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

// The warp scheduler of one frontend slice: the state of the slice's warps
// (wid = {wfs, slice_id}), its warp selection and a fetch request to the
// slice's fetch unit, which names warps by wfs. FU block port q carries the
// traffic of the local warps with wfs mod PORTS == q, addressed by core-wide wid;
// core-wide events arrive through sched_if already localized.
module VX_warp_sched import VX_gpu_pkg::*; #(
    parameter `STRING INSTANCE_ID = ""
) (
    input wire              clk,
    input wire              reset,

    input wire [CORE_ID_WIDTH-1:0] core_id,
    input wire [FETCH_FSW_W-1:0] slice_id,

    VX_slice_sched_if.slave sched_if,

    // ALU ports
    input sched_branch_t [FETCH_ALU_PORTS-1:0] branch,

    // SFU ports
    input sched_wctl_t [FETCH_SFU_PORTS-1:0] wctl,
    input wire [FETCH_SFU_PORTS-1:0][NW_WIDTH-1:0] dvstack_wid,
    output wire [FETCH_SFU_PORTS-1:0][DV_STACK_SIZEW-1:0] dvstack_ptr,
    output wire [FETCH_SFU_PORTS-1:0][`VX_CFG_MEM_ADDR_WIDTH-1:0] dvstack_mscratch,
    input wire [FETCH_SFU_PORTS-1:0][NW_WIDTH-1:0] csr_rd_wid,
    output wire sched_csr_rd_t [FETCH_SFU_PORTS-1:0] csr_rd,
    input sched_csr_wr_t [FETCH_SFU_PORTS-1:0] csr_wr,
`ifdef VX_CFG_EXT_RTU_ENABLE
    input wire [FETCH_SFU_PORTS-1:0] rtu_unlock_valid,
    input wire [FETCH_SFU_PORTS-1:0][NW_WIDTH-1:0] rtu_unlock_wid,
`endif

    // per-warp pipeline events, indexed by wfs
    VX_decode_sched_if.slave decode_sched_if,
    input wire [PER_FETCH_WARPS-1:0] issued,
    input wire [PER_FETCH_WARPS-1:0] committed,
    input wire [PER_FETCH_WARPS-1:0] ibuf_pop,
    output wire [PER_FETCH_WARPS-1:0] pending_alm_empty,

    // fetch request
    VX_schedule_if.master   schedule_if
);
    `UNUSED_SPARAM (INSTANCE_ID)

    localparam SW      = PER_FETCH_WARPS;
    localparam LW      = FETCH_WFS_W;
    localparam NPA     = FETCH_ALU_PORTS;
    localparam NPS     = FETCH_SFU_PORTS;
    localparam NPS_BITS = `CLOG2(NPS);
    localparam PSW     = SW / NPS;           // warps per SFU port
    localparam KW      = `LOG2UP(PSW);
    // with fetch ahead the count must hold the one pick past the buffer's slots
    localparam IBUF_CW = $clog2(`VX_CFG_IBUF_SIZE + 1 + FETCH_AHEAD);
    localparam IFL_W   = $clog2(FETCH_INFLIGHT + 1);

    `STATIC_ASSERT((FETCH_INFLIGHT >= 1) && `IS_POW2(FETCH_INFLIGHT), ("invalid parameter: VX_CFG_FETCH_INFLIGHT must be a power of two"))
`ifdef VX_CFG_EXT_C_ENABLE
    `STATIC_ASSERT(!FETCH_AHEAD, ("invalid parameter: VX_CFG_FETCH_INFLIGHT > 1 fetches 4-byte words ahead, which compressed instructions break"))
    `STATIC_ASSERT(`VX_CFG_FETCH_REQ_BUF != 0, ("invalid parameter: VX_CFG_FETCH_REQ_BUF=0 puts the decompressor's buffer match on the pick's ready"))
`endif

    `UNUSED_VAR (schedule_if.ibuf_pop)

    // wfs -> SFU port-local index
    function automatic logic [KW-1:0] wfs_to_k(input logic [LW-1:0] wfs);
        wfs_to_k = KW'(wfs >> NPS_BITS);
    endfunction

    wire [LW-1:0] dec_lid = decode_sched_if.wid[LW-1:0];
    if (NW_WIDTH > LW) begin : g_dec_wid_unused
        `UNUSED_VAR (decode_sched_if.wid[NW_WIDTH-1:LW])
    end

    wire [NPA-1:0][LW-1:0] br_lid;
    for (genvar q = 0; q < NPA; ++q) begin : g_br_lid
        assign br_lid[q] = wid_to_wfs(branch[q].wid);
        `UNUSED_VAR (wid_to_fsw(branch[q].wid))
    end

    wire [NPS-1:0][LW-1:0] ctl_lid, csr_rd_lid, csr_wr_lid;
`ifdef VX_CFG_EXT_RTU_ENABLE
    wire [NPS-1:0][LW-1:0] rtu_lid;
`endif

    // split/join, one stack per SFU port over the port's warps
    wire [NPS-1:0]                join_valid;
    wire [NPS-1:0]                join_is_dvg;
    wire [NPS-1:0]                join_is_else;
    wire [NPS-1:0][KW-1:0]        join_k;
    wire [NPS-1:0][`VX_CFG_NUM_THREADS-1:0] join_tmask;
    wire [NPS-1:0][PC_BITS-1:0]   join_pc;

    for (genvar q = 0; q < NPS; ++q) begin : g_sfu_ports
        assign ctl_lid[q]    = wid_to_wfs(wctl[q].wid);
        assign csr_rd_lid[q] = wid_to_wfs(csr_rd_wid[q]);
        assign csr_wr_lid[q] = wid_to_wfs(csr_wr[q].wid);
        `UNUSED_VAR ({wid_to_fsw(wctl[q].wid), wid_to_fsw(csr_rd_wid[q]), wid_to_fsw(csr_wr[q].wid), wid_to_fsw(dvstack_wid[q])})
    `ifdef VX_CFG_EXT_RTU_ENABLE
        assign rtu_lid[q]    = wid_to_wfs(rtu_unlock_wid[q]);
        `UNUSED_VAR (wid_to_fsw(rtu_unlock_wid[q]))
    `endif

        VX_split_join #(
            .INSTANCE_ID ((NPS > 1) ? `SFORMATF(("%s-splitjoin%0d", INSTANCE_ID, q)) : `SFORMATF(("%s-splitjoin", INSTANCE_ID))),
            .NUM_WARPS   (PSW),
            .WID_W       (KW),
            .OUT_REG     (1)
        ) split_join (
            .clk         (clk),
            .reset       (reset),
            .split_valid (wctl[q].split_valid),
            .sjoin_valid (wctl[q].sjoin_valid),
            .wid         (wfs_to_k(ctl_lid[q])),
            .split       (wctl[q].split),
            .sjoin       (wctl[q].sjoin),
            .join_valid  (join_valid[q]),
            .join_is_dvg (join_is_dvg[q]),
            .join_is_else(join_is_else[q]),
            .join_wid    (join_k[q]),
            .join_tmask  (join_tmask[q]),
            .join_pc     (join_pc[q]),
            .stack_wid   (wfs_to_k(wid_to_wfs(dvstack_wid[q]))),
            .stack_ptr   (dvstack_ptr[q])
        );
    end

    wire [SW-1:0] active_warps, stalled_warps;
    wire [SW-1:0][`VX_CFG_NUM_THREADS-1:0] thread_masks;
    wire [SW-1:0][PC_BITS-1:0] warp_pcs;
    wire [SW-1:0] ibuf_full, ibuf_full_n;
    wire [SW-1:0] pending_empty;
    wire [SW-1:0] cand_onehot;
    wire          cand_valid;
    wire          sched_fire;

    // fetch ahead, per warp: no fetch in flight; one more may be picked ahead
    wire [SW-1:0] fetch_idle, fetch_ahead_ok;
    wire fetch_spec_t [SW-1:0] fetch_specs;

    reg [SW-1:0][`VX_CFG_MEM_ADDR_WIDTH-1:0] mscratch_r;
    reg [SW-1:0][`VX_CFG_XLEN-1:0] mstatus_r, mtvec_r, mepc_r, mcause_r, mtval_r;

    for (genvar l = 0; l < SW; ++l) begin : g_warps
        localparam [LW-1:0] LID = LW'(l);
        localparam QA = l % NPA;
        localparam QS = l % NPS;
        localparam [KW-1:0] K = KW'(l / NPS);

        reg active_r, active_n;
        reg stalled_r, stalled_n;
        reg [`VX_CFG_NUM_THREADS-1:0] tmask_r, tmask_n;
        reg [PC_BITS-1:0] pc_r, pc_n;

        wire ctl_hit  = (ctl_lid[QS] == LID);
        wire br_hit   = branch[QA].valid && (br_lid[QA] == LID);
        wire join_hit = join_valid[QS] && (join_k[QS] == K);

        always @(*) begin
            active_n  = active_r;
            stalled_n = stalled_r;
            tmask_n   = tmask_r;
            pc_n      = pc_r;

            if (sched_if.cta_fire && (sched_if.cta_lid == LID)) begin
                active_n = 1;
                // Reusing a warp for the next CTA skips the one-time prologue and rewinds to the
                // kernel's per-CTA dispatch window: a fixed 20-byte (5-instruction) sequence that
                // reloads the entry pointer and kargs before re-calling.
                pc_n    = sched_if.cta_init ? sched_if.cta_PC : (pc_r - from_fullPC(`VX_CFG_XLEN'(20)));
                tmask_n = sched_if.cta_tmask;
            end

            if (!FETCH_AHEAD && decode_sched_if.valid && decode_sched_if.unlock && (dec_lid == LID)) begin
                stalled_n = 0;
            end

            if (sched_if.wspawn_apply) begin
                if (sched_if.wspawn_wmask[l]) begin
                    active_n = 1;
                    if (!(sched_if.wspawn_src && (sched_if.wspawn_src_lid == LID))) begin
                        tmask_n[0] = 1;
                        pc_n = sched_if.wspawn_pc;
                    end
                end
                if (sched_if.wspawn_src && (sched_if.wspawn_src_lid == LID)) begin
                    stalled_n = 0;
                end
            end

            if (wctl[QS].tmc_valid && ctl_hit) begin
                active_n  = (wctl[QS].tmc.tmask != 0);
                tmask_n   = wctl[QS].tmc.tmask;
                stalled_n = 0;
            end

            if (wctl[QS].split_valid && ctl_hit) begin
                if (wctl[QS].split.is_dvg) begin
                    tmask_n = wctl[QS].split.then_tmask;
                end
                stalled_n = 0;
            end

            if (join_hit) begin
                if (join_is_dvg[QS]) begin
                    if (join_is_else[QS]) begin
                        pc_n = join_pc[QS];
                    end
                    tmask_n = join_tmask[QS];
                end
                stalled_n = 0;
            end

            if (sched_if.bar_unlock_valid && sched_if.bar_unlock_mask[l]) begin
                stalled_n = 0;
            end

            // an exiting warp is retired as its CTA accounting takes it, before
            // the slot can be relaunched
            if (sched_if.retire_valid && (sched_if.retire_lid == LID)) begin
                active_n  = 0;
                tmask_n   = '0;
                stalled_n = 0;
            end

            // wsync unlock: warp pipeline drained
            if (wctl[QS].wsync_valid && ctl_hit) begin
                stalled_n = 0;
            end

            if (br_hit) begin
                if (branch[QA].is_trap) begin
                    // ECALL/EBREAK: redirect to trap vector (mtvec[1:0] = MODE field; mask off to get base address).
                    pc_n = from_fullPC(mtvec_r[l] & ~`VX_CFG_XLEN'(3));
                end else if (branch[QA].is_mret) begin
                    // MRET/SRET/URET: restore the saved PC from mepc.
                    pc_n = from_fullPC(mepc_r[l]);
                end else if (branch[QA].taken) begin
                    pc_n = branch[QA].dest;
                end
                stalled_n = 0;
            end

            // stall the warp until decode stage; with fetch ahead it stays
            // schedulable, and decode stalls it at a stalling instruction
            if (sched_fire && cand_onehot[l]) begin
                if (!FETCH_AHEAD) begin
                    stalled_n = 1;
                end
            `ifndef VX_CFG_EXT_C_ENABLE
                pc_n = pc_r + from_fullPC(`VX_CFG_XLEN'(4));
            `endif
            end

            // A squash restarts the warp's stream at the PC decode gives: behind a
            // stalling instruction, which also holds the warp, or at the fetch
            // another one overtook. It arrives a cycle after decode, so a pick
            // in between is of the old stream.
            if (FETCH_AHEAD && decode_sched_if.valid && decode_sched_if.squash && (dec_lid == LID)) begin
                pc_n = decode_sched_if.PC;
                if (~decode_sched_if.drop) begin
                    stalled_n = 1;
                end
            end

        `ifdef VX_CFG_EXT_C_ENABLE
            // Every redirect source stalls its warp from schedule until it resolves, so
            // no redirect writes a warp's PC on the cycle that warp decode-advances.
            if (decode_sched_if.valid && (dec_lid == LID)) begin
                pc_n = pc_r + from_fullPC(decode_sched_if.is_rvc ? `VX_CFG_XLEN'(2) : `VX_CFG_XLEN'(4));
            end
        `endif

        `ifdef VX_CFG_EXT_RTU_ENABLE
            // A wstall'd TRACE retires: resume the warp so it proceeds to WAIT.
            if (rtu_unlock_valid[QS] && (rtu_lid[QS] == LID)) begin
                stalled_n = 1'b0;
            end
        `endif
        end

        always @(posedge clk) begin
            if (reset) begin
                active_r      <= 0;
                stalled_r     <= 0;
                tmask_r       <= '0;
                pc_r          <= '0;
                mscratch_r[l] <= '0;
                mstatus_r[l]  <= '0;
                mtvec_r[l]    <= '0;
                mepc_r[l]     <= '0;
                mcause_r[l]   <= '0;
                mtval_r[l]    <= '0;
            end else begin
                active_r  <= active_n;
                stalled_r <= stalled_n;
                tmask_r   <= tmask_n;
                pc_r      <= pc_n;

                // spawned warps inherit the spawning warp's mscratch
                if (sched_if.wspawn_apply && sched_if.wspawn_wmask[l]
                 && !(sched_if.wspawn_src && (sched_if.wspawn_src_lid == LID))) begin
                    mscratch_r[l] <= sched_if.wspawn_mscratch;
                end
                // CTA dispatch: latch this warp's mscratch (param)
                if (sched_if.cta_fire && (sched_if.cta_lid == LID)) begin
                    mscratch_r[l] <= sched_if.cta_param;
                end
                if (csr_wr[QS].valid && (csr_wr_lid[QS] == LID)) begin
                    mscratch_r[l] <= csr_wr[QS].data;
                end

                if (csr_wr_lid[QS] == LID) begin
                    if (csr_wr[QS].trap_valid[0]) begin
                        mstatus_r[l] <= csr_wr[QS].trap_data;
                    end
                    if (csr_wr[QS].trap_valid[1]) begin
                        mtvec_r[l] <= csr_wr[QS].trap_data;
                    end
                    if (csr_wr[QS].trap_valid[2]) begin
                        mepc_r[l] <= csr_wr[QS].trap_data;
                    end
                    if (csr_wr[QS].trap_valid[3]) begin
                        mcause_r[l] <= csr_wr[QS].trap_data;
                    end
                    if (csr_wr[QS].trap_valid[4]) begin
                        mtval_r[l] <= csr_wr[QS].trap_data;
                    end
                end

                // Hardware trap entry (ECALL/EBREAK) wins a same-cycle software write.
                if (br_hit && branch[QA].is_trap) begin
                    mepc_r[l]   <= to_fullPC(branch[QA].dest);
                    mcause_r[l] <= `VX_CFG_XLEN'(branch[QA].trap_cause);
                    mtval_r[l]  <= '0;
                end
            end
        end

        assign active_warps[l]  = active_r;
        assign stalled_warps[l] = stalled_r;
        assign thread_masks[l]  = tmask_r;
        assign warp_pcs[l]      = pc_r;

        // ibuffer occupancy: registered full keeps arbitration off the critical path
        logic [IBUF_CW-1:0] ibuf_size_r, ibuf_size_n;
        logic ibuf_full_r;
        wire ibuf_incr = sched_fire && cand_onehot[l];
        // a dropped fetch returns the slot it was picked for
        wire ibuf_drop = FETCH_AHEAD && decode_sched_if.valid && decode_sched_if.drop && (dec_lid == LID);
        assign ibuf_size_n = ibuf_size_r + IBUF_CW'(ibuf_incr) - IBUF_CW'(ibuf_pop[l]) - IBUF_CW'(ibuf_drop);
        always @(posedge clk) begin
            if (reset) begin
                ibuf_size_r <= '0;
                ibuf_full_r <= 1'b0;
            end else begin
                ibuf_size_r <= ibuf_size_n;
                ibuf_full_r <= ibuf_full_n[l];
            end
        end
        // a warp picked once every buffer is full goes past its slots, and with
        // fetch ahead must read as full there
        assign ibuf_full_n[l] = FETCH_AHEAD ? (ibuf_size_n >= IBUF_CW'(`VX_CFG_IBUF_SIZE))
                                            : (ibuf_size_n == IBUF_CW'(`VX_CFG_IBUF_SIZE));
        assign ibuf_full[l]   = ibuf_full_r;

        if (FETCH_AHEAD) begin : g_fetch_ahead
            // Fetches between pick and decode, the position of the next one in
            // its run, the run, the warp's stream, and whether fetches of an
            // older stream may still be in flight: the stream id is one bit,
            // so a squashed warp is picked again only once they have all left.
            reg [IFL_W-1:0]        inflight_r;
            reg [FETCH_SEQ_W-1:0]  seq_r;
            reg                    run_r;
            reg [FETCH_STRM_W-1:0] strm_r;
            reg                    drain_r;

            wire fetch_pick   = sched_fire && cand_onehot[l];
            wire fetch_leave  = decode_sched_if.valid && (dec_lid == LID);
            wire fetch_squash = fetch_leave && decode_sched_if.squash;

            always @(posedge clk) begin
                if (reset) begin
                    inflight_r <= '0;
                    seq_r      <= '0;
                    run_r      <= 1'b0;
                    strm_r     <= '0;
                    drain_r    <= 1'b0;
                end else begin
                    inflight_r <= inflight_r + IFL_W'(fetch_pick) - IFL_W'(fetch_leave);
                    if (fetch_pick) begin
                        seq_r   <= fetch_idle[l] ? FETCH_SEQ_W'(1) : (seq_r + FETCH_SEQ_W'(1));
                        run_r   <= run_r ^ fetch_idle[l];
                        drain_r <= 1'b0;
                    end
                    if (fetch_squash) begin
                        strm_r  <= strm_r + FETCH_STRM_W'(1);
                        drain_r <= 1'b1;
                    end
                end
            end

        `ifdef VX_CFG_VM_ENABLE
            // A fetch ahead stays in the page of the fetch before it: one that is
            // later dropped must not take a translation fault, which the MMU
            // reports and never answers.
            wire page_ok = (pc_r[`VX_VM_PAGE_LOG2_SIZE-(`VX_CFG_XLEN-PC_BITS)-1:0] != '0);
        `else
            wire page_ok = 1'b1;
        `endif

            assign fetch_idle[l]     = (inflight_r == '0);
            assign fetch_ahead_ok[l] = ~drain_r && (inflight_r != IFL_W'(FETCH_INFLIGHT)) && page_ok;
            assign fetch_specs[l]    = '{seq: fetch_idle[l] ? FETCH_SEQ_W'(0) : seq_r, strm: strm_r,
                                         run: run_r ^ fetch_idle[l], first: fetch_idle[l]};

            `RUNTIME_ASSERT(~fetch_leave || (inflight_r != '0),
                ("%t: %s fetch left with none in flight, wid=%0d", $time, INSTANCE_ID, wfs_to_wid(LID, slice_id)))
        end else begin : g_fetch_single
            assign fetch_idle[l]     = 1'b1;
            assign fetch_ahead_ok[l] = 1'b0;
            assign fetch_specs[l]    = '0;
        end

        VX_pending_size #(
            .SIZE      (256),
            .ALM_EMPTY (1)
        ) pending_ctr (
            .clk       (clk),
            .reset     (reset),
            .incr      (issued[l]),
            .decr      (committed[l]),
            .empty     (pending_empty[l]),
            .alm_empty (pending_alm_empty[l]),
            `UNUSED_PIN (full),
            `UNUSED_PIN (alm_full),
            `UNUSED_PIN (size)
        );

    `ifdef DBG_TRACE_PIPELINE
        always @(posedge clk) begin
            if (active_n != active_r
             || (active_r && (stalled_n != stalled_r || tmask_n != tmask_r))) begin
                `TRACE(1, ("%t: %s warp-state: wid=%0d, active=%b, stalled=%b, tmask=%b\n",
                    $time, INSTANCE_ID, wfs_to_wid(LID, slice_id), active_n, stalled_n, tmask_n))
            end
        end
    `endif
    end

    // the slice's lowest ready warp
    wire [SW-1:0] ready_warps     = active_warps & ~stalled_warps;
    wire [SW-1:0] preferred_warps = ready_warps & ~ibuf_full;
`ifndef L1_ENABLE
    // without L1, the icache must never stall: it shares the bus with dcache responses
    wire [SW-1:0] full_warps     = preferred_warps;
    wire [SW-1:0] schedule_warps = preferred_warps;
    `UNUSED_VAR (sched_if.slice_ibuf_full)
`else
    // once every ibuffer of the core is full, any ready warp may fetch
    wire [SW-1:0] full_warps     = ready_warps;
    wire [SW-1:0] schedule_warps = (& sched_if.slice_ibuf_full) ? full_warps : preferred_warps;
`endif

    // With fetch ahead a warp with a fetch in flight is not picked again, unless
    // it is the only runnable warp of the slice, has room in its window and its
    // buffer, and no squash to drain. With other warps runnable they fill its
    // gaps, and the fetches it would drop at its next branch would take icache
    // bandwidth from them.
    wire [SW-1:0] pick_warps, pick_room_warps, pick_full_warps;
    if (FETCH_AHEAD) begin : g_pick_ahead
        wire one_ready = ((ready_warps & (ready_warps - SW'(1))) == '0);
        wire [SW-1:0] ahead_warps = preferred_warps & fetch_ahead_ok & {SW{one_ready}};
        assign pick_room_warps = (preferred_warps & fetch_idle) | ahead_warps;
        assign pick_full_warps = (full_warps & fetch_idle) | ahead_warps;
        assign pick_warps      = (schedule_warps & fetch_idle) | ahead_warps;
    end else begin : g_pick_single
        assign pick_room_warps = preferred_warps;
        assign pick_full_warps = full_warps;
        assign pick_warps      = schedule_warps;
        `UNUSED_VAR ({fetch_idle, fetch_ahead_ok})
        `UNUSED_VAR ({decode_sched_if.drop, decode_sched_if.squash, decode_sched_if.PC})
    end

    wire [`UP(FETCH_WFS_BITS)-1:0] cand_index;
    VX_priority_encoder #(
        .N (SW)
    ) wid_select (
        .data_in   (pick_warps),
        .index_out (cand_index),
        .valid_out (cand_valid),
        .onehot_out(cand_onehot)
    );
    wire [LW-1:0] cand_lid = LW'(cand_index);

    // CTA CSRs and CTA ids, one store per SFU port over the port's warps
    wire [NPS-1:0][NCTA_WIDTH-1:0] port_cta_id;

    for (genvar q = 0; q < NPS; ++q) begin : g_csr_cta
        wire [LW-1:0] warp_waddr = sched_if.warp_waddr;
        wire [LW-1:0] id_waddr   = sched_if.id_waddr;

        VX_csr_cta #(
            .NUM_WARPS (PSW)
        ) csr_cta (
            .clk          (clk),
            .reset        (reset),
            .warp_write   (sched_if.warp_write && ((32'(warp_waddr) % NPS) == q)),
            .warp_waddr   (wfs_to_k(warp_waddr)),
            .warp_wdata   (sched_if.warp_wdata),
            .id_write     (sched_if.id_write && ((32'(id_waddr) % NPS) == q)),
            .id_waddr     (wfs_to_k(id_waddr)),
            .id_wdata     (sched_if.id_wdata),
            .rd_lid       (wfs_to_k(csr_rd_lid[q])),
            .rd_cta_rank  (csr_rd[q].cta_rank),
            .rd_lane      (csr_rd[q].cta_lane),
            .sched_lid    (wfs_to_k(cand_lid)),
            .sched_cta_id (port_cta_id[q])
        );

        assign csr_rd[q].thread_mask = thread_masks[csr_rd_lid[q]];
        assign csr_rd[q].mscratch    = mscratch_r[csr_rd_lid[q]];
        assign csr_rd[q].mstatus     = mstatus_r[csr_rd_lid[q]];
        assign csr_rd[q].mtvec       = mtvec_r[csr_rd_lid[q]];
        assign csr_rd[q].mepc        = mepc_r[csr_rd_lid[q]];
        assign csr_rd[q].mcause      = mcause_r[csr_rd_lid[q]];
        assign csr_rd[q].mtval       = mtval_r[csr_rd_lid[q]];

        assign dvstack_mscratch[q]   = mscratch_r[wid_to_wfs(dvstack_wid[q])];
    end

    wire [NCTA_WIDTH-1:0] cand_cta_id = port_cta_id[32'(cand_lid) % NPS];

    // fetch request, registered at the slice boundary; without the buffer the
    // pick goes straight to the fetch unit's request buffer, whose ready is a
    // register
    wire sched_ready;
    assign sched_fire = cand_valid && sched_ready;

    wire [UUID_WIDTH-1:0] sched_uuid;
`ifdef UUID_ENABLE
    VX_uuid_gen #(
        .NUM_WARPS (SW)
    ) uuid_gen (
        .clk     (clk),
        .reset   (reset),
        .core_id (core_id),
        .incr    (sched_fire),
        .wid     (wfs_to_wid(cand_lid, slice_id)),
        .uuid    (sched_uuid)
    );
`else
    assign sched_uuid = '0;
    `UNUSED_VAR (core_id)
`ifndef DBG_TRACE_PIPELINE
    `UNUSED_VAR (slice_id)
`endif
`endif

    localparam SCHED_DATAW = `VX_CFG_NUM_THREADS + PC_BITS + NW_WIDTH + NCTA_WIDTH + UUID_WIDTH;
    localparam SPEC_DATAW  = FETCH_AHEAD ? $bits(fetch_spec_t) : 0;

    wire [SCHED_DATAW+SPEC_DATAW-1:0] sched_data_in, sched_data_out;
    wire [SCHED_DATAW-1:0] cand_data = {thread_masks[cand_lid], warp_pcs[cand_lid], NW_WIDTH'(cand_lid), cand_cta_id, sched_uuid};
    if (FETCH_AHEAD) begin : g_sched_spec
        assign sched_data_in    = {fetch_specs[cand_lid], cand_data};
        assign schedule_if.spec = sched_data_out[SCHED_DATAW +: SPEC_DATAW];
    end else begin : g_sched_nospec
        assign sched_data_in    = cand_data;
        assign schedule_if.spec = '0;
        `UNUSED_VAR (fetch_specs)
    end
    assign {schedule_if.data.tmask, schedule_if.data.PC, schedule_if.data.wid, schedule_if.data.cta_id, schedule_if.data.uuid} = sched_data_out[SCHED_DATAW-1:0];

    VX_elastic_buffer #(
        .DATAW   (SCHED_DATAW + SPEC_DATAW),
        .SIZE    ((`VX_CFG_FETCH_REQ_BUF != 0) ? 2 : 0),
        .OUT_REG (1)
    ) sched_buf (
        .clk       (clk),
        .reset     (reset),
        .valid_in  (cand_valid),
        .ready_in  (sched_ready),
        .data_in   (sched_data_in),
        .data_out  (sched_data_out),
        .valid_out (schedule_if.valid),
        .ready_out (schedule_if.ready)
    );

    wire schedule_if_fire = schedule_if.valid && schedule_if.ready;

    // status
    assign sched_if.active        = active_warps;
    assign sched_if.stalled       = stalled_warps;
    assign sched_if.ibuf_full     = (& ibuf_full);
    assign sched_if.pending_empty = pending_empty;

    // performance events
    for (genvar q = 0; q < NPA; ++q) begin : g_branch_valid
        assign sched_if.branch_valid[q] = branch[q].valid;
    end
    for (genvar q = 0; q < NPS; ++q) begin : g_divergence
        assign sched_if.divergence[q] = wctl[q].split_valid && wctl[q].split.is_dvg;
    end
    assign sched_if.sched_valid_room = (| pick_room_warps);
    assign sched_if.sched_valid_full = (| pick_full_warps);
    assign sched_if.sched_fire    = schedule_if_fire;
    assign sched_if.sched_tmask   = schedule_if.data.tmask;

`ifdef EXT_SCHED_TIMEOUT_DUMP
    always @(posedge clk) begin
        if (sched_if.timeout_dump) begin
            for (integer l = 0; l < SW; ++l) begin
                $display("    wid=%0d stalled=%0d pc=0x%0h tmask=%b",
                         wfs_to_wid(LW'(l), slice_id), stalled_warps[l], to_fullPC(warp_pcs[l]), thread_masks[l]);
            end
        end
    end
`endif

`ifdef DBG_TRACE_PIPELINE
    always @(posedge clk) begin
        if (sched_fire) begin
            `TRACE(1, ("%t: %s dispatch: wid=%0d, cta_id=%0d, PC=0x%0h, tmask=%b (#%0d)\n", $time, INSTANCE_ID,
                wfs_to_wid(cand_lid, slice_id), cand_cta_id, to_fullPC(warp_pcs[cand_lid]), thread_masks[cand_lid], sched_uuid))
        end
    end
`endif

endmodule
