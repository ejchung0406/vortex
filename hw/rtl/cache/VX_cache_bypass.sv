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

`include "VX_cache_define.vh"

module VX_cache_bypass import VX_gpu_pkg::*; #(
    parameter NUM_REQS          = 1,
    parameter MEM_PORTS         = 1,
    parameter TAG_SEL_IDX       = 0,

    parameter CACHE_ENABLE      = 0,
    parameter bit SLICE_BIND        = 0,
    parameter SLICE_GRANULE     = 1,

    parameter WORD_SIZE         = 1,
    parameter LINE_SIZE         = 1,

    parameter CORE_ADDR_WIDTH   = 1,

    parameter CORE_TAG_WIDTH    = 1,

    parameter MEM_ADDRW    = 1,
    parameter MEM_TAG_IN_WIDTH  = 1,

    parameter CORE_OUT_BUF      = 0,
    parameter MEM_OUT_BUF       = 0
 ) (
    input wire clk,
    input wire reset,

    VX_mem_bus_if.slave     core_bus_in_if [NUM_REQS],
    VX_mem_bus_if.master    core_bus_out_if [NUM_REQS],
    VX_mem_bus_if.slave     mem_bus_in_if [MEM_PORTS],
    VX_mem_bus_if.master    mem_bus_out_if [MEM_PORTS],

    // no uncached or outgoing request held short of the memory side
    output wire             req_empty
);
    localparam DIRECT_PASSTHRU   = !CACHE_ENABLE && (`CS_WORD_SEL_BITS == 0) && (NUM_REQS == MEM_PORTS);
    localparam CORE_DATA_WIDTH   = WORD_SIZE * 8;
    localparam WORDS_PER_LINE    = LINE_SIZE / WORD_SIZE;
    localparam WSEL_BITS         = `CLOG2(WORDS_PER_LINE);
    localparam CORE_TAG_ID_WIDTH = CORE_TAG_WIDTH - UUID_WIDTH;
    localparam NC_SRC_BITS       = SLICE_BIND ? `CLOG2(NUM_REQS) : `CLOG2(`CDIV(NUM_REQS, MEM_PORTS));
    localparam MEM_TAG_ID_WIDTH  = NC_SRC_BITS + CORE_TAG_ID_WIDTH;
    localparam MEM_TAG_NC1_WIDTH = UUID_WIDTH + MEM_TAG_ID_WIDTH;
    localparam MEM_TAG_NC2_WIDTH = MEM_TAG_NC1_WIDTH + WSEL_BITS;
    localparam MEM_TAG_OUT_WIDTH = CACHE_ENABLE ? `MAX(MEM_TAG_IN_WIDTH, MEM_TAG_NC2_WIDTH) : MEM_TAG_NC2_WIDTH;
    // Beside a cache, the memory-side arbiter grants on valid, so spreading
    // uncached requests over more memory ports by their ready would close a
    // valid-ready loop: with fewer core ports, core port i keeps memory port i.
    localparam NC_PORTS          = (CACHE_ENABLE && !SLICE_BIND) ? `MIN(NUM_REQS, MEM_PORTS) : MEM_PORTS;
    if (SLICE_BIND && !CACHE_ENABLE) begin : g_invalid_slice_binding
        `ELABORATION_ERROR("L2 slice binding requires an enabled cache")
    end
    `STATIC_ASSERT(0 == (`VX_MEM_IO_BASE_ADDR % `VX_CFG_MEM_BLOCK_SIZE), ("invalid parameter"))
    VX_mem_bus_if #(
        .DATA_SIZE (WORD_SIZE),
        .TAG_WIDTH (CORE_TAG_WIDTH)
    ) core_bus_nc_switch_if[(CACHE_ENABLE ? 2 : 1) * NUM_REQS]();
    wire [NUM_REQS-1:0] core_req_nc_sel;
    for (genvar i = 0; i < NUM_REQS; ++i) begin : g_core_req_is_nc
        if (CACHE_ENABLE) begin : g_cache
            // a flush token always reaches the cache, whatever its address
            assign core_req_nc_sel[i] = ~core_bus_in_if[i].req_data.attr[MEM_ATTR_IO_OFFS]
                                      | core_bus_in_if[i].req_data.attr[MEM_ATTR_FLUSH_OFFS];
        end else begin : g_no_cache
            assign core_req_nc_sel[i] = 1'b0;
        end
    end
    if (SLICE_BIND) begin : g_bound_core_switch
        for (genvar i = 0; i < NUM_REQS; ++i) begin : g_slot
            VX_mem_bus_switch #(
                .NUM_INPUTS  (1),
                .NUM_OUTPUTS (2),
                .DATA_SIZE   (WORD_SIZE),
                .TAG_WIDTH   (CORE_TAG_WIDTH),
                .ARBITER     ("R"),
                .REQ_OUT_BUF (0),
                .RSP_OUT_BUF (CORE_OUT_BUF)
            ) core_bus_nc_switch (
                .clk        (clk),
                .reset      (reset),
                .bus_sel    (core_req_nc_sel[i +: 1]),
                .bus_in_if  (core_bus_in_if[i +: 1]),
                .bus_out_if (core_bus_nc_switch_if[2*i +: 2])
            );
        end
    end else begin : g_legacy_core_switch
        VX_mem_bus_switch #(
            .NUM_INPUTS  (NUM_REQS),
            .NUM_OUTPUTS ((CACHE_ENABLE ? 2 : 1) * NUM_REQS),
            .DATA_SIZE   (WORD_SIZE),
            .TAG_WIDTH   (CORE_TAG_WIDTH),
            .ARBITER     ("R"),
            .REQ_OUT_BUF (0),
            .RSP_OUT_BUF (DIRECT_PASSTHRU ? 0 : CORE_OUT_BUF)
        ) core_bus_nc_switch (
            .clk       (clk),
            .reset     (reset),
            .bus_sel   (core_req_nc_sel),
            .bus_in_if (core_bus_in_if),
            .bus_out_if(core_bus_nc_switch_if)
        );
    end
    VX_mem_bus_if #(
        .DATA_SIZE (WORD_SIZE),
        .TAG_WIDTH (CORE_TAG_WIDTH)
    ) core_bus_in_nc_if[NUM_REQS]();
    for (genvar i = 0; i < NUM_REQS; ++i) begin : g_core_bus_nc_switch_if
        localparam NC_IDX = SLICE_BIND ? (2*i) : i;
        localparam CACHE_IDX = SLICE_BIND ? (2*i+1) : (NUM_REQS+i);
        assign core_bus_in_nc_if[i].req_valid = core_bus_nc_switch_if[NC_IDX].req_valid;
        assign core_bus_in_nc_if[i].req_data  = core_bus_nc_switch_if[NC_IDX].req_data;
        assign core_bus_nc_switch_if[NC_IDX].req_ready = core_bus_in_nc_if[i].req_ready;
        assign core_bus_nc_switch_if[NC_IDX].rsp_valid = core_bus_in_nc_if[i].rsp_valid;
        assign core_bus_nc_switch_if[NC_IDX].rsp_data  = core_bus_in_nc_if[i].rsp_data;
        assign core_bus_in_nc_if[i].rsp_ready = core_bus_nc_switch_if[NC_IDX].rsp_ready;
        if (CACHE_ENABLE) begin : g_cache
            assign core_bus_out_if[i].req_valid = core_bus_nc_switch_if[CACHE_IDX].req_valid;
            assign core_bus_out_if[i].req_data  = core_bus_nc_switch_if[CACHE_IDX].req_data;
            assign core_bus_nc_switch_if[CACHE_IDX].req_ready = core_bus_out_if[i].req_ready;
            assign core_bus_nc_switch_if[CACHE_IDX].rsp_valid = core_bus_out_if[i].rsp_valid;
            assign core_bus_nc_switch_if[CACHE_IDX].rsp_data  = core_bus_out_if[i].rsp_data;
            assign core_bus_out_if[i].rsp_ready = core_bus_nc_switch_if[CACHE_IDX].rsp_ready;
        end else begin : g_no_cache
            `INIT_VX_MEM_BUS_IF (core_bus_out_if[i])
        end
    end
    VX_mem_bus_if #(
        .DATA_SIZE (WORD_SIZE),
        .TAG_WIDTH (MEM_TAG_NC1_WIDTH)
    ) core_bus_nc_arb_if[NC_PORTS]();
    if (SLICE_BIND) begin : g_bound_nc
        localparam SRC_WIDTH = `UP(NC_SRC_BITS);
        localparam CHANNEL_BITS = `CLOG2(MEM_PORTS);
        localparam CHANNEL_WIDTH = `UP(CHANNEL_BITS);
        localparam GRANULE_BITS = `CLOG2(SLICE_GRANULE);
        localparam REQ_DATAW = 1 + CORE_ADDR_WIDTH + CORE_DATA_WIDTH + WORD_SIZE + MEM_ATTR_WIDTH + CORE_TAG_WIDTH;
        localparam RSP_DATAW = CORE_DATA_WIDTH + CORE_TAG_WIDTH;

        wire [NUM_REQS-1:0] req_valid_in, req_ready_in;
        wire [NUM_REQS-1:0][REQ_DATAW-1:0] req_data_in;
        wire [NUM_REQS-1:0][CHANNEL_WIDTH-1:0] req_channel;
        wire [MEM_PORTS-1:0] req_valid_out, req_ready_out;
        wire [MEM_PORTS-1:0][REQ_DATAW-1:0] req_data_out;
        wire [MEM_PORTS-1:0][SRC_WIDTH-1:0] req_source;

        wire [MEM_PORTS-1:0] rsp_valid_in, rsp_ready_in;
        wire [MEM_PORTS-1:0][RSP_DATAW-1:0] rsp_data_in;
        wire [MEM_PORTS-1:0][SRC_WIDTH-1:0] rsp_source;
        wire [NUM_REQS-1:0] rsp_valid_out, rsp_ready_out;
        wire [NUM_REQS-1:0][RSP_DATAW-1:0] rsp_data_out;

        for (genvar i = 0; i < NUM_REQS; ++i) begin : g_source
            wire [63:0] byte_addr = 64'(core_bus_in_nc_if[i].req_data.addr) << `CLOG2(WORD_SIZE);
            assign req_channel[i] = CHANNEL_WIDTH'(l2_slice_channel(byte_addr, GRANULE_BITS, CHANNEL_BITS));
            assign req_valid_in[i] = core_bus_in_nc_if[i].req_valid;
            assign req_data_in[i] = core_bus_in_nc_if[i].req_data;
            assign core_bus_in_nc_if[i].req_ready = req_ready_in[i];

            assign core_bus_in_nc_if[i].rsp_valid = rsp_valid_out[i];
            assign core_bus_in_nc_if[i].rsp_data = rsp_data_out[i];
            assign rsp_ready_out[i] = core_bus_in_nc_if[i].rsp_ready;
        end

        for (genvar i = 0; i < MEM_PORTS; ++i) begin : g_channel
            assign core_bus_nc_arb_if[i].req_valid = req_valid_out[i];
            assign req_ready_out[i] = core_bus_nc_arb_if[i].req_ready;

            assign rsp_valid_in[i] = core_bus_nc_arb_if[i].rsp_valid;
            assign core_bus_nc_arb_if[i].rsp_ready = rsp_ready_in[i];
            if (NC_SRC_BITS != 0) begin : g_source_tag
                // A source can issue to any channel, so the tag carries its full index.
                assign core_bus_nc_arb_if[i].req_data = {req_data_out[i], req_source[i]};
                assign rsp_source[i] = SRC_WIDTH'(core_bus_nc_arb_if[i].rsp_data.tag);
                assign rsp_data_in[i] = {core_bus_nc_arb_if[i].rsp_data.data,
                                         CORE_TAG_WIDTH'(core_bus_nc_arb_if[i].rsp_data.tag >> NC_SRC_BITS)};
            end else begin : g_one_source
                `UNUSED_VAR (req_source[i])
                assign core_bus_nc_arb_if[i].req_data = req_data_out[i];
                assign rsp_source[i] = '0;
                assign rsp_data_in[i] = core_bus_nc_arb_if[i].rsp_data;
            end
        end

        VX_stream_xbar #(
            .NUM_INPUTS  (NUM_REQS),
            .NUM_OUTPUTS (MEM_PORTS),
            .DATAW       (REQ_DATAW),
            .ARBITER     ("R"),
            .OUT_BUF     (3),
            .MAX_FANOUT  (0)
        ) nc_req_xbar (
            .clk       (clk),
            .reset     (reset),
            .valid_in  (req_valid_in),
            .data_in   (req_data_in),
            .sel_in    (req_channel),
            .ready_in  (req_ready_in),
            .valid_out (req_valid_out),
            .data_out  (req_data_out),
            .sel_out   (req_source),
            .ready_out (req_ready_out),
            `UNUSED_PIN (collisions)
        );

        VX_stream_xbar #(
            .NUM_INPUTS  (MEM_PORTS),
            .NUM_OUTPUTS (NUM_REQS),
            .DATAW       (RSP_DATAW),
            .OUT_BUF     (3),
            .MAX_FANOUT  (0)
        ) nc_rsp_xbar (
            .clk       (clk),
            .reset     (reset),
            .valid_in  (rsp_valid_in),
            .data_in   (rsp_data_in),
            .sel_in    (rsp_source),
            .ready_in  (rsp_ready_in),
            .valid_out (rsp_valid_out),
            .data_out  (rsp_data_out),
            `UNUSED_PIN (sel_out),
            .ready_out (rsp_ready_out),
            `UNUSED_PIN (collisions)
        );
    end else begin : g_legacy_nc
        VX_mem_bus_arb #(
            .NUM_INPUTS (NUM_REQS),
            .NUM_OUTPUTS(NC_PORTS),
            .DATA_SIZE  (WORD_SIZE),
            .TAG_WIDTH  (CORE_TAG_WIDTH),
            .TAG_SEL_IDX(TAG_SEL_IDX),
            .ARBITER    (CACHE_ENABLE ? "P" : "R"),
            .REQ_OUT_BUF(0),
            .RSP_OUT_BUF(0)
        ) core_bus_nc_arb (
            .clk        (clk),
            .reset      (reset),
            .bus_in_if  (core_bus_in_nc_if),
            .bus_out_if (core_bus_nc_arb_if)
        );
    end
    VX_mem_bus_if #(
        .DATA_SIZE (LINE_SIZE),
        .TAG_WIDTH (MEM_TAG_NC2_WIDTH)
    ) mem_bus_out_nc_if[MEM_PORTS]();
    for (genvar i = 0; i < NC_PORTS; ++i) begin : g_mem_bus_out_nc
        wire                        core_req_nc_arb_rw;
        wire [WORD_SIZE-1:0]        core_req_nc_arb_byteen;
        wire [CORE_ADDR_WIDTH-1:0]  core_req_nc_arb_addr;
        wire [MEM_ATTR_WIDTH-1:0] core_req_nc_arb_attr;
        wire [CORE_DATA_WIDTH-1:0]  core_req_nc_arb_data;
        wire [MEM_TAG_NC1_WIDTH-1:0] core_req_nc_arb_tag;
        assign {
            core_req_nc_arb_rw,
            core_req_nc_arb_addr,
            core_req_nc_arb_data,
            core_req_nc_arb_byteen,
            core_req_nc_arb_attr,
            core_req_nc_arb_tag
        } = core_bus_nc_arb_if[i].req_data;
        logic [MEM_ADDRW-1:0] core_req_nc_arb_addr_w;
        logic [WORDS_PER_LINE-1:0][WORD_SIZE-1:0] core_req_nc_arb_byteen_w;
        logic [WORDS_PER_LINE-1:0][CORE_DATA_WIDTH-1:0] core_req_nc_arb_data_w;
        logic [CORE_DATA_WIDTH-1:0] core_rsp_nc_arb_data_w;
        wire [MEM_TAG_NC2_WIDTH-1:0] core_req_nc_arb_tag_w;
        wire [MEM_TAG_NC1_WIDTH-1:0] core_rsp_nc_arb_tag_w;
        if (WORDS_PER_LINE > 1) begin : g_multi_word_line
            wire [WSEL_BITS-1:0] rsp_wsel;
            wire [WSEL_BITS-1:0] req_wsel = core_req_nc_arb_addr[WSEL_BITS-1:0];
            always @(*) begin
                core_req_nc_arb_byteen_w = '0;
                core_req_nc_arb_byteen_w[req_wsel] = core_req_nc_arb_byteen;
                core_req_nc_arb_data_w = 'x;
                core_req_nc_arb_data_w[req_wsel] = core_req_nc_arb_data;
            end
            VX_bits_insert #(
                .N   (MEM_TAG_NC1_WIDTH),
                .S   (WSEL_BITS),
                .POS (TAG_SEL_IDX)
            ) wsel_insert (
                .data_in  (core_req_nc_arb_tag),
                .ins_in   (req_wsel),
                .data_out (core_req_nc_arb_tag_w)
            );
            VX_bits_remove #(
                .N   (MEM_TAG_NC1_WIDTH + WSEL_BITS),
                .S   (WSEL_BITS),
                .POS (TAG_SEL_IDX)
            ) wsel_remove (
                .data_in  (mem_bus_out_nc_if[i].rsp_data.tag),
                .sel_out  (rsp_wsel),
                .data_out (core_rsp_nc_arb_tag_w)
            );
            assign core_req_nc_arb_addr_w   = core_req_nc_arb_addr[WSEL_BITS +: MEM_ADDRW];
            assign core_rsp_nc_arb_data_w   = mem_bus_out_nc_if[i].rsp_data.data[rsp_wsel * CORE_DATA_WIDTH +: CORE_DATA_WIDTH];
        end else begin : g_single_word_line
            assign core_req_nc_arb_addr_w   = core_req_nc_arb_addr;
            assign core_req_nc_arb_byteen_w = core_req_nc_arb_byteen;
            assign core_req_nc_arb_data_w   = core_req_nc_arb_data;
            assign core_req_nc_arb_tag_w    = MEM_TAG_NC2_WIDTH'(core_req_nc_arb_tag);
            assign core_rsp_nc_arb_data_w   = mem_bus_out_nc_if[i].rsp_data.data;
            assign core_rsp_nc_arb_tag_w    = MEM_TAG_NC1_WIDTH'(mem_bus_out_nc_if[i].rsp_data.tag);
        end
        assign mem_bus_out_nc_if[i].req_valid = core_bus_nc_arb_if[i].req_valid;
        assign mem_bus_out_nc_if[i].req_data = {
            core_req_nc_arb_rw,
            core_req_nc_arb_addr_w,
            core_req_nc_arb_data_w,
            core_req_nc_arb_byteen_w,
            core_req_nc_arb_attr,
            core_req_nc_arb_tag_w
        };
        assign core_bus_nc_arb_if[i].req_ready = mem_bus_out_nc_if[i].req_ready;
        assign core_bus_nc_arb_if[i].rsp_valid = mem_bus_out_nc_if[i].rsp_valid;
        assign core_bus_nc_arb_if[i].rsp_data = {
            core_rsp_nc_arb_data_w,
            core_rsp_nc_arb_tag_w
        };
        assign mem_bus_out_nc_if[i].rsp_ready = core_bus_nc_arb_if[i].rsp_ready;
    end
    for (genvar i = NC_PORTS; i < MEM_PORTS; ++i) begin : g_mem_bus_out_nc_idle
        `INIT_VX_MEM_BUS_IF (mem_bus_out_nc_if[i])
    end
    VX_mem_bus_if #(
        .DATA_SIZE (LINE_SIZE),
        .TAG_WIDTH (MEM_TAG_OUT_WIDTH)
    ) mem_bus_out_src_if[(CACHE_ENABLE ? 2 : 1) * MEM_PORTS]();
    for (genvar i = 0; i < MEM_PORTS; ++i) begin : g_mem_bus_out_src
        localparam NC_IDX = SLICE_BIND ? (2*i) : i;
        localparam CACHE_IDX = SLICE_BIND ? (2*i+1) : (MEM_PORTS+i);
        `ASSIGN_VX_MEM_BUS_IF_EX(mem_bus_out_src_if[NC_IDX], mem_bus_out_nc_if[i], MEM_TAG_OUT_WIDTH, MEM_TAG_NC2_WIDTH, UUID_WIDTH);
        if (CACHE_ENABLE) begin : g_cache
            `ASSIGN_VX_MEM_BUS_IF_EX(mem_bus_out_src_if[CACHE_IDX], mem_bus_in_if[i], MEM_TAG_OUT_WIDTH, MEM_TAG_IN_WIDTH, UUID_WIDTH);
        end else begin : g_no_cache
            `UNUSED_VX_MEM_BUS_IF(mem_bus_in_if[i])
        end
    end
    if (SLICE_BIND) begin : g_bound_mem_arb
        for (genvar i = 0; i < MEM_PORTS; ++i) begin : g_channel
            VX_mem_bus_arb #(
                .NUM_INPUTS (2),
                .NUM_OUTPUTS(1),
                .DATA_SIZE  (LINE_SIZE),
                .TAG_WIDTH  (MEM_TAG_OUT_WIDTH),
                .ARBITER    ("R"),
                .REQ_OUT_BUF(MEM_OUT_BUF),
                .RSP_OUT_BUF(0)
            ) mem_bus_out_arb (
                .clk        (clk),
                .reset      (reset),
                .bus_in_if  (mem_bus_out_src_if[2*i +: 2]),
                .bus_out_if (mem_bus_out_if[i +: 1])
            );
        end
    end else begin : g_legacy_mem_arb
        VX_mem_bus_arb #(
            .NUM_INPUTS ((CACHE_ENABLE ? 2 : 1) * MEM_PORTS),
            .NUM_OUTPUTS(MEM_PORTS),
            .DATA_SIZE  (LINE_SIZE),
            .TAG_WIDTH  (MEM_TAG_OUT_WIDTH),
            .ARBITER    ("R"),
            .REQ_OUT_BUF(DIRECT_PASSTHRU ? 0 : MEM_OUT_BUF),
            .RSP_OUT_BUF(0)
        ) mem_bus_out_arb (
            .clk        (clk),
            .reset      (reset),
            .bus_in_if  (mem_bus_out_src_if),
            .bus_out_if (mem_bus_out_if)
        );
    end

    if (!DIRECT_PASSTHRU) begin : g_req_pending
        localparam NUM_SRCS = NUM_REQS + (CACHE_ENABLE ? MEM_PORTS : 0);
        wire [NUM_SRCS-1:0]  src_fire;
        wire [MEM_PORTS-1:0] dst_fire;
        for (genvar i = 0; i < NUM_REQS; ++i) begin : g_nc_fire
            assign src_fire[i] = core_bus_in_nc_if[i].req_valid && core_bus_in_nc_if[i].req_ready;
        end
        if (CACHE_ENABLE) begin : g_cache_fire
            for (genvar i = 0; i < MEM_PORTS; ++i) begin : g_mem_in_fire
                assign src_fire[NUM_REQS + i] = mem_bus_in_if[i].req_valid && mem_bus_in_if[i].req_ready;
            end
        end
        for (genvar i = 0; i < MEM_PORTS; ++i) begin : g_dst_fire
            assign dst_fire[i] = mem_bus_out_if[i].req_valid && mem_bus_out_if[i].req_ready;
        end
        VX_mem_req_pending #(
            .NUM_INPUTS  (NUM_SRCS),
            .NUM_OUTPUTS (MEM_PORTS)
        ) req_pending (
            .clk      (clk),
            .reset    (reset),
            .in_fire  (src_fire),
            .out_fire (dst_fire),
            .empty    (req_empty)
        );
    end else begin : g_no_req_pending
        assign req_empty = 1'b1;
    end
endmodule
