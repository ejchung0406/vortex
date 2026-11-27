// Copyright © 2019-2023
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

`include "VX_define.vh"

module VX_cluster import VX_gpu_pkg::*, VX_tlb_pkg::*;
#(
    parameter `STRING INSTANCE_ID = ""
) (
    `SCOPE_IO_DECL

    // Clock
    input  wire                 clk,
    input  wire                 reset,

    input  wire [CLUSTER_ID_WIDTH-1:0] cluster_id,

`ifdef PERF_ENABLE
    input sysmem_perf_t         sysmem_perf,
`endif

    // DCRs
    VX_dcr_bus_if.slave         dcr_bus_if,

    // Memory
    VX_mem_bus_if.master        mem_bus_if [L2_MEM_PORTS],

    // KMU bus
    VX_kmu_bus_if.slave         kmu_bus_if[1],
    // This cluster's cores masked off for launches
    input  wire [`VX_CFG_NUM_CORES-1:0] core_disable,
    input  wire core_mask_active,

    // Global barrier bus to the device-level barrier unit
    VX_gbar_bus_if.master       gbar_bus_if,

`ifdef VX_CFG_EXT_RASTER_ENABLE
    // Delegated draw launch (device KMU → raster engines)
    VX_raster_launch_if.slave   raster_launch_if[1],
`endif

`ifdef VX_CFG_VM_ENABLE
    // Device-level walker: the L2 TLB's miss bus, exported to the device.
    VX_tlb_bus_if.master           dev_ptw_if,
    // Walker PTE fetches, entering as an L2 client when this cluster's L2
    // is the LLC (PTW_ON_L2); tied off otherwise.
    VX_mem_bus_if.slave            dev_ptw_mem_if,
    input  wire                    mmu_flush_req,
    output wire                    mmu_flush_done,
`ifdef CLUSTER_UNIT_MMU_ENABLE
    // Translation active vs BARE (satp.MODE), for the cluster unit MMU.
    input  wire                    mmu_vm_active,
`endif
`endif

    // Command-processor DMA, entering as an L2 client when this cluster's L2
    // is the LLC (CP_ON_L2); tied off otherwise.
    VX_mem_bus_if.slave         cp_mem_if,

    // Status
    output wire                 busy
);

`ifdef SCOPE
    localparam scope_socket = 0;
`ifdef EXT_GFX_CLUSTER_ENABLE
    localparam scope_gfx = NUM_SOCKETS;
    `SCOPE_IO_SWITCH (NUM_SOCKETS + 1);
`else
    `SCOPE_IO_SWITCH (NUM_SOCKETS);
`endif
`endif

`ifdef PERF_ENABLE
    cache_perf_t l2_perf;
    sysmem_perf_t sysmem_perf_tmp;
`ifdef VX_CFG_EXT_RASTER_ENABLE
    raster_perf_t gfx_raster_perf;
    cache_perf_t  gfx_rcache_perf;
`endif
`ifdef VX_CFG_EXT_OM_ENABLE
    om_perf_t    gfx_om_perf;
    cache_perf_t gfx_ocache_perf;
`endif
    always @(*) begin
        sysmem_perf_tmp = sysmem_perf;
        sysmem_perf_tmp.l2cache = l2_perf;
    `ifdef VX_CFG_EXT_RASTER_ENABLE
        sysmem_perf_tmp.raster = gfx_raster_perf;
        sysmem_perf_tmp.rcache = gfx_rcache_perf;
    `endif
    `ifdef VX_CFG_EXT_OM_ENABLE
        sysmem_perf_tmp.om     = gfx_om_perf;
        sysmem_perf_tmp.ocache = gfx_ocache_perf;
    `endif
    end
`endif

    VX_kmu_bus_if per_socket_kmu_bus_if[NUM_SOCKETS]();

    // The mask crosses the chip from the KMU, so it is registered on arrival.
    // It changes only on a DCR write, which precedes the launch it applies to
    // by many cycles, so the cycle of lag never reaches a CTA.
    wire [`VX_CFG_NUM_CORES-1:0] core_disable_r;
    wire core_mask_active_r;
    `BUFFER(core_disable_r, core_disable);
    `BUFFER(core_mask_active_r, core_mask_active);

    // Each socket's enabled cores weight its share of compute launches.
    wire [NUM_SOCKETS-1:0][`CLOG2(KMU_DEST_DIV_CLUSTER+1)-1:0] per_socket_cores;
    for (genvar i = 0; i < NUM_SOCKETS; ++i) begin : g_socket_cores
        assign per_socket_cores[i] = `CLOG2(KMU_DEST_DIV_CLUSTER+1)'($countones(~core_disable_r[i * `VX_CFG_SOCKET_SIZE +: `VX_CFG_SOCKET_SIZE]));
    end

`ifdef VX_CFG_EXT_RASTER_ENABLE
    // The raster engines are a launch master alongside the device KMU: a fragment
    // wave IS a launch now, so it merges here instead of being injected at every
    // core behind a private data bus.
    VX_kmu_bus_if raster_kmu_bus_if[1]();

    VX_kmu_bus_if kmu_arb_in_if[2]();

    assign kmu_arb_in_if[0].valid = kmu_bus_if[0].valid;
    assign kmu_arb_in_if[0].data  = kmu_bus_if[0].data;
    assign kmu_arb_in_if[0].kind  = kmu_bus_if[0].kind;
    assign kmu_arb_in_if[0].eop   = kmu_bus_if[0].eop;
    assign kmu_arb_in_if[0].dest  = kmu_bus_if[0].dest;
    assign kmu_bus_if[0].ready    = kmu_arb_in_if[0].ready;

    assign kmu_arb_in_if[1].valid = raster_kmu_bus_if[0].valid;
    assign kmu_arb_in_if[1].data  = raster_kmu_bus_if[0].data;
    assign kmu_arb_in_if[1].kind  = raster_kmu_bus_if[0].kind;
    assign kmu_arb_in_if[1].eop   = raster_kmu_bus_if[0].eop;
    assign kmu_arb_in_if[1].dest  = raster_kmu_bus_if[0].dest;
    assign raster_kmu_bus_if[0].ready = kmu_arb_in_if[1].ready;

    // Merge {compute, raster} onto one launch stream (fan-in message lock), then
    // distribute it to sockets (kind routing + fan-out lock). Two single-stage arbs
    // rather than one 2->N crossbar: the boundary between them is this registered,
    // cluster-visible launch link, so back-pressure never crosses both stages
    // combinationally and the in-transit beat stays visible to busy.
    VX_kmu_bus_if cluster_merged_kmu_if[1]();

    VX_kmu_bus_arb #(
        .NUM_INPUTS  (2),
        .NUM_OUTPUTS (1),
        .ARBITER     ("R"),
        .OUT_BUF     (3)   // register the merged launch link
    ) kmu_merge (
        .clk         (clk),
        .reset       (reset),
        .bus_in_if   (kmu_arb_in_if),
        .bus_out_if  (cluster_merged_kmu_if),
        .out_cores   (1'b1),
        .mask_active (1'b0)
    );

    VX_kmu_bus_arb #(
        .NUM_INPUTS  (1),
        .NUM_OUTPUTS (NUM_SOCKETS),
        .DEST_DIV    (KMU_DEST_DIV_CLUSTER),
        .OUT_BUF     ((NUM_SOCKETS > 1) ? 3 : 0)
    ) kmu_dist (
        .clk         (clk),
        .reset       (reset),
        .bus_in_if   (cluster_merged_kmu_if),
        .bus_out_if  (per_socket_kmu_bus_if),
        .out_cores   (per_socket_cores),
        .mask_active (core_mask_active_r)
    );
`else
    VX_kmu_bus_arb #(
        .NUM_INPUTS (1),
        .NUM_OUTPUTS (NUM_SOCKETS),
        .DEST_DIV   (KMU_DEST_DIV_CLUSTER),
        .OUT_BUF    ((NUM_SOCKETS > 1) ? 3 : 0)
    ) kmu_arb (
        .clk         (clk),
        .reset       (reset),
        .bus_in_if   (kmu_bus_if),
        .bus_out_if  (per_socket_kmu_bus_if),
        .out_cores   (per_socket_cores),
        .mask_active (core_mask_active_r)
    );
`endif

    VX_gbar_bus_if per_socket_gbar_bus_if[NUM_SOCKETS]();

    VX_gbar_arb #(
        .NUM_REQS (NUM_SOCKETS),
        .REQ_OUT_BUF ((NUM_SOCKETS > 1) ? 3 : 0),
        .RSP_OUT_BUF ((NUM_SOCKETS > 1) ? 3 : 0)
    ) gbar_arb (
        .clk        (clk),
        .reset      (reset),
        .bus_in_if  (per_socket_gbar_bus_if),
        .bus_out_if (gbar_bus_if)
    );

    // L2 input buses (socket ports + cluster-resident gfx cache ports)
    VX_mem_bus_if #(
        .DATA_SIZE (`VX_CFG_L1_LINE_SIZE),
        .TAG_WIDTH (L2_TAG_WIDTH)
    ) per_socket_mem_bus_if[L2_CACHE_REQS]();

    // Socket memory output buses (socket arb output tag width)
    VX_mem_bus_if #(
        .DATA_SIZE (`VX_CFG_L1_LINE_SIZE),
        .TAG_WIDTH (SOCKET_MEM_ARB_TAG_WIDTH)
    ) socket_mem_bus_if[SOCKET_MEM_PORTS]();

    // L2 side of the sockets' trunk, ahead of the link to the L2
    VX_mem_bus_if #(
        .DATA_SIZE (`VX_CFG_L1_LINE_SIZE),
        .TAG_WIDTH (SOCKET_MEM_ARB_TAG_WIDTH)
    ) l2_link_bus_if[SOCKET_MEM_PORTS]();

`ifdef VX_CFG_EXT_RASTER_ENABLE
    VX_mem_bus_if #(
        .DATA_SIZE (`VX_CFG_L1_LINE_SIZE),
        .TAG_WIDTH (L2_TAG_WIDTH)
    ) rcache_l2_bus_if();
`endif

`ifdef VX_CFG_EXT_OM_ENABLE
    VX_mem_bus_if #(
        .DATA_SIZE (`VX_CFG_L1_LINE_SIZE),
        .TAG_WIDTH (L2_TAG_WIDTH)
    ) ocache_l2_bus_if();
`endif

    VX_mem_bus_if #(
        .DATA_SIZE (L2_SECTOR_SIZE),
        .TAG_WIDTH (L2_MEM_TAG_WIDTH)
    ) l2_mem_bus_if[L2_MEM_PORTS]();
    wire l2_req_empty;

    VX_cache_wrap #(
        .INSTANCE_ID    (`SFORMATF(("%s-l2cache", INSTANCE_ID))),
        .CACHE_SIZE     (`VX_CFG_L2_SIZE),
        .LINE_SIZE      (`VX_CFG_L2_LINE_SIZE),
        .SECTOR_SIZE    (L2_SECTOR_SIZE),
        .NUM_BANKS      (L2_NUM_BANKS),
        .NUM_WAYS       (`VX_CFG_L2_NUM_WAYS),
        .WORD_SIZE      (L2_WORD_SIZE),
        .NUM_REQS       (L2_CACHE_REQS),
        .MEM_PORTS      (L2_MEM_PORTS),
        .CRSQ_SIZE      (`VX_CFG_L2_CRSQ_SIZE),
        .MSHR_SIZE      (`VX_CFG_L2_MSHR_SIZE),
        .MRSQ_SIZE      (`VX_CFG_L2_MRSQ_SIZE),
        .MREQ_SIZE      (`VX_CFG_L2_MREQ_SIZE),
        .LATENCY        (`VX_CFG_L2_LATENCY),
        .TAG_WIDTH      (L2_TAG_WIDTH),
        .WRITE_ENABLE   (1),
        .WRITEBACK      (`VX_CFG_L2_WRITEBACK),
        .DIRTY_BYTES    (`VX_CFG_L2_DIRTYBYTES),
        .LAZY_FETCH     (`VX_CFG_L2_LAZY_FETCH),
        .WRITE_AROUND   (CP_ON_L2),
        .REPL_POLICY    (`VX_CFG_L2_REPL_POLICY),
        .CORE_OUT_BUF   (3),
        .MEM_OUT_BUF    (3),
        .CORE_REQ_SKID  (1),
        .NC_ENABLE      (1),
        .PASSTHRU       (!`VX_CFG_L2_ENABLED),
        .IS_LLC         (L2_IS_LLC),
        .SHARED_LLC     (L2_SHARED_LLC),
        .BANK_HASH      (`VX_CFG_L2_BANK_HASH),
        .SLICE_BIND     (L2_SLICE_BIND),
        .SLICE_GRANULE  (L2_SLICE_GRANULE),
        .AMO_ENABLE     (`VX_CFG_EXT_A_ENABLED)
    ) l2cache (
        .clk            (clk),
        .reset          (reset),
    `ifdef PERF_ENABLE
        .cache_perf     (l2_perf),
    `endif
        .core_bus_if    (per_socket_mem_bus_if),
        .mem_bus_if     (l2_mem_bus_if),
        .req_empty      (l2_req_empty)
    );

    // Cluster DCR distribution — declared before its consumers (sockets/gfx).
`ifdef EXT_GFX_CLUSTER_ENABLE
    localparam NUM_DCR_GFX = 1;
    localparam DCR_GFX_IDX = NUM_SOCKETS;
`else
    localparam NUM_DCR_GFX = 0;
`endif
    localparam NUM_DCR_REQS = NUM_SOCKETS + NUM_DCR_GFX;
    VX_dcr_bus_if per_socket_dcr_bus_if[NUM_DCR_REQS]();
    VX_dcr_arb #(
        .NUM_REQS    (NUM_DCR_REQS),
        .REQ_OUT_BUF ((NUM_DCR_REQS > 1) ? 1 : 0),
        .RSP_OUT_BUF ((NUM_DCR_REQS > 1) ? 1 : 0)
    ) dcr_socket_arb (
        .clk        (clk),
        .reset      (reset),
        .bus_in_if  (dcr_bus_if),
        .bus_out_if (per_socket_dcr_bus_if)
    );

`ifdef VX_CFG_EXT_OM_ENABLE
    // The socket->L2 trunk runs through VX_graphics, which peels the OM aperture
    // writes off it. The whole OM path -- steer, ingress, arb, cores -- lives there.
    VX_mem_bus_if #(
        .DATA_SIZE (`VX_CFG_L1_LINE_SIZE),
        .TAG_WIDTH (L2_TAG_WIDTH)
    ) gfx_l2_out_bus_if [L2_SOCKET_REQS]();

    for (genvar i = 0; i < L2_SOCKET_REQS; ++i) begin : g_socket_l2
        `ASSIGN_VX_MEM_BUS_IF (l2_link_bus_if[i], gfx_l2_out_bus_if[i]);
    end
`else
    // Sockets connect straight to their L2 input ports (TEX/RTU/DXA traffic
    // already merged at each socket's L2-facing arb).
    for (genvar i = 0; i < SOCKET_MEM_PORTS; ++i) begin : g_socket_l2
        `ASSIGN_VX_MEM_BUS_IF (l2_link_bus_if[i], socket_mem_bus_if[i]);
    end
`endif

    wire l2_link_busy;

    if (`VX_CFG_NOC_GROUP_SIZE != 0) begin : g_noc

        // SM groups: a group's socket ports reach its switch, and the switch
        // reaches the L2 over the group's uplink lanes.
        `STATIC_ASSERT(`IS_DIVISBLE(NUM_SOCKETS, `VX_CFG_NOC_GROUP_SIZE), ("NOC_GROUP_SIZE must divide the socket count"))
        `STATIC_ASSERT(L2_GFX_REQS == 0, ("SM groups do not carry the cluster-resident graphics caches"))

        localparam NOC_LANES = `VX_CFG_NOC_UPLINK_PORTS;

        wire [NOC_NUM_GROUPS-1:0] group_busy;

        for (genvar g = 0; g < NOC_NUM_GROUPS; ++g) begin : g_groups

            VX_mem_bus_if #(
                .DATA_SIZE (`VX_CFG_L1_LINE_SIZE),
                .TAG_WIDTH (SOCKET_MEM_ARB_TAG_WIDTH)
            ) sm_bus_if[NOC_GROUP_PORTS]();

            VX_mem_bus_if #(
                .DATA_SIZE (`VX_CFG_L1_LINE_SIZE),
                .TAG_WIDTH (L2_TAG_WIDTH)
            ) lane_bus_if[NOC_LANES]();

            wire switch_req_empty, switch_rsp_empty;

            for (genvar p = 0; p < NOC_GROUP_PORTS; ++p) begin : g_sm_ports
                `ASSIGN_VX_MEM_BUS_IF (sm_bus_if[p], l2_link_bus_if[g * NOC_GROUP_PORTS + p]);
            end

            wire [NOC_LANES-1:0]                          deliver_valid;
            wire [NOC_LANES-1:0][`UP(NOC_PORT_BITS)-1:0]  deliver_port;
            wire [NOC_LANES-1:0]                          deliver_token;

            VX_noc_switch #(
                .NUM_PORTS (NOC_GROUP_PORTS),
                .NUM_LANES (NOC_LANES),
                .DATA_SIZE (`VX_CFG_L1_LINE_SIZE),
                .TAG_WIDTH (SOCKET_MEM_ARB_TAG_WIDTH)
            ) noc_switch (
                .clk           (clk),
                .reset         (reset),
                .port_bus_if   (sm_bus_if),
                .lane_bus_if   (lane_bus_if),
                .deliver_valid (deliver_valid),
                .deliver_port  (deliver_port),
                .deliver_token (deliver_token),
                .req_empty     (switch_req_empty),
                .rsp_empty     (switch_rsp_empty)
            );

            for (genvar l = 0; l < NOC_LANES; ++l) begin : g_uplink
                localparam LANE = g * NOC_LANES + l;
                `ASSIGN_VX_MEM_BUS_IF (per_socket_mem_bus_if[LANE], lane_bus_if[l]);
                assign deliver_valid[l] = per_socket_mem_bus_if[LANE].req_valid && per_socket_mem_bus_if[LANE].req_ready;
                assign deliver_port[l]  = `UP(NOC_PORT_BITS)'(per_socket_mem_bus_if[LANE].req_data.tag);
                assign deliver_token[l] = per_socket_mem_bus_if[LANE].req_data.attr[MEM_ATTR_FLUSH_OFFS];
            end

            assign group_busy[g] = ~(switch_req_empty && switch_rsp_empty);
        end

        assign l2_link_busy = (| group_busy);

    end else begin : g_l2_links

        // The sockets' link to the L2: L2_LINK_LATENCY cycles each way.
        wire [L2_SOCKET_REQS-1:0] l2_link_empty;
        for (genvar i = 0; i < L2_SOCKET_REQS; ++i) begin : g_l2_link
            if (`VX_CFG_L2_LINK_LATENCY != 0) begin : g_delay
                VX_mem_bus_delay #(
                    .DATA_SIZE (`VX_CFG_L1_LINE_SIZE),
                    .TAG_WIDTH (L2_TAG_WIDTH),
                    .LATENCY   (`VX_CFG_L2_LINK_LATENCY)
                ) link (
                    .clk        (clk),
                    .reset      (reset),
                    .bus_in_if  (l2_link_bus_if[i]),
                    .bus_out_if (per_socket_mem_bus_if[i]),
                    .empty      (l2_link_empty[i]),
                    `UNUSED_PIN (req_empty)
                );
            end else begin : g_wire
                `ASSIGN_VX_MEM_BUS_IF (per_socket_mem_bus_if[i], l2_link_bus_if[i]);
                assign l2_link_empty[i] = 1'b1;
            end
        end
        assign l2_link_busy = ~(& l2_link_empty);

    end

    for (genvar i = 0; i < L2_MEM_PORTS; ++i) begin : g_l2_mem_out
        `ASSIGN_VX_MEM_BUS_IF (mem_bus_if[i], l2_mem_bus_if[i]);
    end

    if (CP_ON_L2 != 0) begin : g_cp_l2_client
        `ASSIGN_VX_MEM_BUS_IF (per_socket_mem_bus_if[L2_CP_IDX], cp_mem_if);
    end else begin : g_no_cp_l2_client
        `UNUSED_VX_MEM_BUS_IF (cp_mem_if)
    end

    ///////////////////////////////////////////////////////////////////////////

    wire [NUM_SOCKETS-1:0] per_socket_busy;

`ifdef EXT_GFX_ANY_ENABLE
    // OR all socket flush reqs together; broadcast .done to every socket.
    // The cores start their flushes together, so the read-only graphics caches
    // flush once for all of them; the OR drops when the last core has finished.
    VX_dcr_flush_if cluster_flush_if();
    VX_dcr_flush_if per_socket_cluster_flush_if [NUM_SOCKETS]();
    wire [NUM_SOCKETS-1:0] per_socket_cluster_flush_req;
    for (genvar s = 0; s < NUM_SOCKETS; ++s) begin : g_per_socket_cluster_flush
        assign per_socket_cluster_flush_req[s]    = per_socket_cluster_flush_if[s].req;
        assign per_socket_cluster_flush_if[s].done = cluster_flush_if.done;
    end
    assign cluster_flush_if.req = (| per_socket_cluster_flush_req);
`endif

`ifdef VX_CFG_VM_ENABLE
    // ---------------------------------------------------------------------
    // Shared cluster TLB + page-table walker
    // ---------------------------------------------------------------------
    // Every socket's tlb_bus, then the cluster unit MMU's (when present).
    VX_tlb_bus_if #(.ID_WIDTH (TLB_SOCKET_ID_WIDTH))
        per_socket_tlb_bus_if [CLUSTER_NUM_TLB_PORTS] ();

    VX_tlb_bus_if #(.ID_WIDTH (TLB_CLUSTER_ID_WIDTH)) l2_client_if ();
    VX_tlb_bus_if #(.ID_WIDTH (L2_TLB_SLOT_WIDTH))    l2_ptw_if ();

    VX_tlb_bus_arb #(
        .NUM_INPUTS  (CLUSTER_NUM_TLB_PORTS),
        .ID_WIDTH_IN (TLB_SOCKET_ID_WIDTH),
        .OUT_BUF     (3)
    ) tlb_cluster_arb (
        .clk        (clk),
        .reset      (reset),
        .bus_in_if  (per_socket_tlb_bus_if),
        .bus_out_if (l2_client_if)
    );

    VX_tlb_flush_if l2_flush_if ();
    wire l2_empty;

    VX_tlb_l2 #(
        .INSTANCE_ID (`SFORMATF(("%s-l2tlb", INSTANCE_ID))),
        .ID_WIDTH    (TLB_CLUSTER_ID_WIDTH)
    ) l2tlb (
        .clk       (clk),
        .reset     (reset),
        .client_if (l2_client_if),
        .ptw_if    (l2_ptw_if),
        .flush_if  (l2_flush_if),
        .empty     (l2_empty)
    );

    // The walker lives at the device: export the L2 TLB's miss bus.
    assign dev_ptw_if.req_valid = l2_ptw_if.req_valid;
    assign dev_ptw_if.req_data  = l2_ptw_if.req_data;
    assign l2_ptw_if.req_ready  = dev_ptw_if.req_ready;
    assign l2_ptw_if.rsp_valid  = dev_ptw_if.rsp_valid;
    assign l2_ptw_if.rsp_data   = dev_ptw_if.rsp_data;
    assign dev_ptw_if.rsp_ready = l2_ptw_if.rsp_ready;

    if (PTW_ON_L2 != 0) begin : g_ptw_l2_client
        `ASSIGN_VX_MEM_BUS_IF (per_socket_mem_bus_if[L2_PTW_IDX], dev_ptw_mem_if);
    end else begin : g_no_ptw_l2_client
        `UNUSED_VX_MEM_BUS_IF (dev_ptw_mem_if)
    end

    // Flush root fans to the cluster L2; each socket self-times its own L1
    // TLB flush off the SATP DCR write, and the device walker reports its
    // own done leg at the top.
    assign l2_flush_if.req  = mmu_flush_req;
`ifdef CLUSTER_UNIT_MMU_ENABLE
    // The rcache and ocache are virtually tagged: the address-space change
    // drains the ocache's writes and invalidates both, then flushes the
    // cluster unit MMU's TLB that those writes went through.
    VX_dcr_flush_if gfx_mmu_flush_if();
    VX_tlb_flush_if cmmu_flush_if();
    wire cmmu_empty;
    assign gfx_mmu_flush_if.req = mmu_flush_req;
    assign cmmu_flush_if.req    = mmu_flush_req && gfx_mmu_flush_if.done;
    assign mmu_flush_done       = l2_flush_if.done && cmmu_flush_if.done;
    wire mmu_busy = ~(l2_empty && cmmu_empty);
`else
    assign mmu_flush_done   = l2_flush_if.done;
    wire mmu_busy = ~l2_empty;
`endif
`else
    wire mmu_busy = 1'b0;
`endif

    for (genvar socket_id = 0; socket_id < NUM_SOCKETS; ++socket_id) begin : g_sockets

        VX_socket #(
            .INSTANCE_ID (`SFORMATF(("%s-socket%0d", INSTANCE_ID, socket_id)))
        ) socket (
            `SCOPE_IO_BIND  (scope_socket+socket_id)

            .clk            (clk),
            .reset          (reset),
            .socket_id      (CORE_ID_WIDTH'(cluster_id) * CORE_ID_WIDTH'(NUM_SOCKETS) + CORE_ID_WIDTH'(socket_id)),

        `ifdef PERF_ENABLE
            .sysmem_perf    (sysmem_perf_tmp),
        `endif

            .dcr_bus_if     (per_socket_dcr_bus_if[socket_id]),

            .mem_bus_if     (socket_mem_bus_if[socket_id * L1_MEM_PORTS +: L1_MEM_PORTS]),

        `ifdef VX_CFG_VM_ENABLE
            .cluster_tlb_bus_if (per_socket_tlb_bus_if[socket_id]),
        `endif

        `ifdef VX_CFG_EXT_OM_ENABLE
        `endif

        `ifdef EXT_GFX_ANY_ENABLE
            .cluster_flush_if (per_socket_cluster_flush_if[socket_id]),
        `endif

            .kmu_bus_if     (per_socket_kmu_bus_if[socket_id +: 1]),
            .core_disable   (core_disable_r[socket_id * `VX_CFG_SOCKET_SIZE +: `VX_CFG_SOCKET_SIZE]),
            .core_mask_active (core_mask_active_r),

            .gbar_bus_if    (per_socket_gbar_bus_if[socket_id]),

            .busy           (per_socket_busy[socket_id])
        );
    end

    ///////////////////////////////////////////////////////////////////////////
    // Graphics extensions cluster integration
    ///////////////////////////////////////////////////////////////////////////

`ifdef EXT_GFX_CLUSTER_ENABLE
    // Producer busy from the graphics block (raster engine out-of-band drain).
    wire gfx_busy;

    // Alias the graphics block's dedicated DCR array element onto a scalar
    // interface: a constant array index in a modport binding is rejected by
    // sv2v. Pure net joins, zero cost.
    VX_dcr_bus_if gfx_dcr_bus_if();
    assign gfx_dcr_bus_if.req_valid                     = per_socket_dcr_bus_if[DCR_GFX_IDX].req_valid;
    assign gfx_dcr_bus_if.req_data                      = per_socket_dcr_bus_if[DCR_GFX_IDX].req_data;
    assign per_socket_dcr_bus_if[DCR_GFX_IDX].rsp_valid = gfx_dcr_bus_if.rsp_valid;
    assign per_socket_dcr_bus_if[DCR_GFX_IDX].rsp_data  = gfx_dcr_bus_if.rsp_data;

    VX_graphics #(
        .INSTANCE_ID (INSTANCE_ID)
    ) graphics (
        `SCOPE_IO_BIND (scope_gfx)
        .clk        (clk),
        .reset      (reset),
        .cluster_id (cluster_id),
    `ifdef PERF_ENABLE
    `ifdef VX_CFG_EXT_RASTER_ENABLE
        .raster_perf              (gfx_raster_perf),
        .rcache_perf              (gfx_rcache_perf),
    `endif
    `ifdef VX_CFG_EXT_OM_ENABLE
        .om_perf                  (gfx_om_perf),
        .ocache_perf              (gfx_ocache_perf),
    `endif
    `endif
    `ifdef VX_CFG_EXT_RASTER_ENABLE
        .raster_kmu_bus_if (raster_kmu_bus_if),
        .rcache_mem_bus_if        (rcache_l2_bus_if),
        .raster_launch_if         (raster_launch_if),
    `endif
    `ifdef VX_CFG_EXT_OM_ENABLE
        .socket_mem_bus_if        (socket_mem_bus_if),
        .l2_out_bus_if            (gfx_l2_out_bus_if),
        .ocache_mem_bus_if        (ocache_l2_bus_if),
    `endif
        .dcr_bus_if               (gfx_dcr_bus_if),
        .cluster_flush_if         (cluster_flush_if),
    `ifdef CLUSTER_UNIT_MMU_ENABLE
        .mmu_flush_if             (gfx_mmu_flush_if),
    `endif
        .busy                     (gfx_busy)
    );

`ifdef CLUSTER_UNIT_MMU_ENABLE
    ///////////////////////////////////////////////////////////////////////////
    // Cluster unit MMU — translates the rcache and ocache memory ports on
    // their way to the L2, so those caches are virtually indexed and tagged.
    // Its misses go straight to the cluster TLB as one more client.
    ///////////////////////////////////////////////////////////////////////////

    VX_mem_bus_if #(
        .DATA_SIZE (`VX_CFG_L1_LINE_SIZE),
        .TAG_WIDTH (L2_TAG_WIDTH)
    ) gfx_mem_bus_if[CMMU_NUM_REQS]();

    VX_mem_bus_if #(
        .DATA_SIZE (`VX_CFG_L1_LINE_SIZE),
        .TAG_WIDTH (L2_TAG_WIDTH)
    ) gfx_phys_bus_if[CMMU_NUM_REQS]();

`ifdef VX_CFG_EXT_RASTER_ENABLE
    `ASSIGN_VX_MEM_BUS_IF (gfx_mem_bus_if[CMMU_RCACHE_IDX], rcache_l2_bus_if);
    `ASSIGN_VX_MEM_BUS_IF (per_socket_mem_bus_if[L2_GFX_RASTER_IDX], gfx_phys_bus_if[CMMU_RCACHE_IDX]);
`endif
`ifdef VX_CFG_EXT_OM_ENABLE
    `ASSIGN_VX_MEM_BUS_IF (gfx_mem_bus_if[CMMU_OCACHE_IDX], ocache_l2_bus_if);
    `ASSIGN_VX_MEM_BUS_IF (per_socket_mem_bus_if[L2_GFX_OM_IDX], gfx_phys_bus_if[CMMU_OCACHE_IDX]);
`endif

    // Its miss bus enters the cluster TLB arbiter after the sockets' buses,
    // its slot id zero-extended to their width.
    VX_tlb_bus_if #(
        .ID_WIDTH (L1_TLB_ID_WIDTH)
    ) cmmu_tlb_bus_if();
    assign per_socket_tlb_bus_if[NUM_SOCKETS].req_valid       = cmmu_tlb_bus_if.req_valid;
    assign per_socket_tlb_bus_if[NUM_SOCKETS].req_data.id     = `UP(TLB_SOCKET_ID_WIDTH)'(cmmu_tlb_bus_if.req_data.id);
    assign per_socket_tlb_bus_if[NUM_SOCKETS].req_data.access = cmmu_tlb_bus_if.req_data.access;
    assign per_socket_tlb_bus_if[NUM_SOCKETS].req_data.amo    = cmmu_tlb_bus_if.req_data.amo;
    assign per_socket_tlb_bus_if[NUM_SOCKETS].req_data.vpn    = cmmu_tlb_bus_if.req_data.vpn;
    assign cmmu_tlb_bus_if.req_ready = per_socket_tlb_bus_if[NUM_SOCKETS].req_ready;
    assign cmmu_tlb_bus_if.rsp_valid = per_socket_tlb_bus_if[NUM_SOCKETS].rsp_valid;
    assign cmmu_tlb_bus_if.rsp_data.id    = `UP(L1_TLB_ID_WIDTH)'(per_socket_tlb_bus_if[NUM_SOCKETS].rsp_data.id);
    assign cmmu_tlb_bus_if.rsp_data.fault = per_socket_tlb_bus_if[NUM_SOCKETS].rsp_data.fault;
    assign cmmu_tlb_bus_if.rsp_data.level = per_socket_tlb_bus_if[NUM_SOCKETS].rsp_data.level;
    assign cmmu_tlb_bus_if.rsp_data.ppn   = per_socket_tlb_bus_if[NUM_SOCKETS].rsp_data.ppn;
    assign cmmu_tlb_bus_if.rsp_data.flags = per_socket_tlb_bus_if[NUM_SOCKETS].rsp_data.flags;
    assign per_socket_tlb_bus_if[NUM_SOCKETS].rsp_ready = cmmu_tlb_bus_if.rsp_ready;

    VX_mmu_fault_if cmmu_fault_if();
    // Faults are reported by the walker, as for the socket MMUs.
    `UNUSED_VAR (cmmu_fault_if.valid)
    `UNUSED_VAR (cmmu_fault_if.va)
    `UNUSED_VAR (cmmu_fault_if.access)
    `UNUSED_VAR (cmmu_fault_if.amo)

`ifdef PERF_ENABLE
    mmu_perf_t cmmu_perf;
    `UNUSED_VAR (cmmu_perf)
`endif

    VX_mmu #(
        .INSTANCE_ID  ("cmmu"),
        .NUM_REQS     (CMMU_NUM_REQS),
        .TLB_SIZE     (`VX_CFG_CTLB_SIZE),
        .EXEC_SIDE    (0),
        .WRITE_ENABLE (`VX_CFG_EXT_OM_ENABLED),
        .RSP_OUT_BUF  (2),
        .PARK_REG     (1),
        .DATA_SIZE    (`VX_CFG_L1_LINE_SIZE),
        .TAG_WIDTH    (L2_TAG_WIDTH)
    ) cmmu (
        .clk         (clk),
        .reset       (reset),
        .vm_active   (mmu_vm_active),
    `ifdef PERF_ENABLE
        .mmu_perf    (cmmu_perf),
    `endif
        .core_bus_if (gfx_mem_bus_if),
        .mem_bus_if  (gfx_phys_bus_if),
        .tlb_bus_if  (cmmu_tlb_bus_if),
        .flush_if    (cmmu_flush_if),
        .fault_if    (cmmu_fault_if),
        `UNUSED_PIN (lane_idle),
        .empty       (cmmu_empty)
    );
`else
`ifdef VX_CFG_EXT_RASTER_ENABLE
    `ASSIGN_VX_MEM_BUS_IF (per_socket_mem_bus_if[L2_GFX_RASTER_IDX], rcache_l2_bus_if);
`endif

`ifdef VX_CFG_EXT_OM_ENABLE
    `ASSIGN_VX_MEM_BUS_IF (per_socket_mem_bus_if[L2_GFX_OM_IDX], ocache_l2_bus_if);
`endif
`endif // CLUSTER_UNIT_MMU_ENABLE

`elsif EXT_GFX_ANY_ENABLE
    // No cluster-resident graphics caches: the socket-local flush legs
    // (tcache/rtcache) complete on their own; this level contributes done=1.
    assign cluster_flush_if.done = 1'b1;
    `UNUSED_VAR (cluster_flush_if.req)
    wire gfx_busy = 1'b0;
`endif // EXT_GFX_CLUSTER_ENABLE

    // Launch liveness: fold this level's launch-link `valid` at both ends into busy
    // combinationally -- the beat presented at the cluster input and any beat resident
    // in a per-socket output skid -- so a launch in transit is never invisible on the
    // presented cycle. Child (per_socket_busy) stays registered.
    // A socket's busy has no path from its inputs: the launch and DCR beats this
    // level presents to the sockets join the registered aggregation here.
    wire [NUM_SOCKETS-1:0] per_socket_kmu_valid;
    wire [NUM_SOCKETS-1:0] per_socket_dcr_valid;
    for (genvar i = 0; i < NUM_SOCKETS; ++i) begin : g_kmu_link_valid
        assign per_socket_kmu_valid[i] = per_socket_kmu_bus_if[i].valid;
        assign per_socket_dcr_valid[i] = per_socket_dcr_bus_if[i].req_valid;
    end
    wire socket_issued_busy = (| per_socket_kmu_valid) | (| per_socket_dcr_valid);
`ifdef VX_CFG_EXT_RASTER_ENABLE
    // The merge->distribute link is a registered cluster-internal launch bus; a beat
    // resident in it is covered here just like the input and per-socket links.
    wire launch_link_busy = kmu_bus_if[0].valid | cluster_merged_kmu_if[0].valid | (|per_socket_kmu_valid);
`else
    wire launch_link_busy = kmu_bus_if[0].valid | (|per_socket_kmu_valid);
`endif

    wire busy_r;
`ifdef EXT_GFX_ANY_ENABLE
    `BUFFER_EX(busy_r, dcr_bus_if.req_valid | (|per_socket_busy) | socket_issued_busy | gfx_busy | mmu_busy | l2_link_busy | ~l2_req_empty, 1'b1, 1, (NUM_SOCKETS > 1));
`else
    `BUFFER_EX(busy_r, dcr_bus_if.req_valid | (|per_socket_busy) | socket_issued_busy | mmu_busy | l2_link_busy | ~l2_req_empty, 1'b1, 1, (NUM_SOCKETS > 1));
`endif
    assign busy = busy_r | dcr_bus_if.req_valid | launch_link_busy;

endmodule
