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

// DXA shared-memory write arbiter, one bank row per cycle (DXA_LMEM_ROW).
//
// A worker's beat covers one or more line-wide parts of a bank row. The
// round-robin picks a lead beat; each part the lead leaves empty then takes
// the beat of the first worker after the lead that lies wholly in that part,
// targets the same core and does not complete a transfer (a beat carrying the
// completion goes alone, so a write carries at most one). The parts are
// written at their own rows: part 0 at the request address, part p at the
// address carried in the attribute above the completion bits. Each bank is
// written at most once per cycle.

`include "VX_define.vh"

module VX_dxa_lmem_arb import VX_gpu_pkg::*; #(
    parameter NUM_INPUTS  = 1,
    parameter PARTS       = 2,
    parameter DATA_SIZE   = 1,
    parameter ADDR_WIDTH  = 1,
    parameter TAG_WIDTH   = 1,
    parameter TAG_SEL_IDX = 0,
    // completion bits {last, bar} at the bottom of the attribute
    parameter DONE_WIDTH  = 1,
    parameter ATTR_WIDTH  = DONE_WIDTH + (PARTS - 1) * ADDR_WIDTH,
    parameter REQ_OUT_BUF = 3
) (
    input wire clk,
    input wire reset,

    VX_mem_bus_if.slave  bus_in_if [NUM_INPUTS],
    VX_mem_bus_if.master bus_out_if [1]
);
    `STATIC_ASSERT ((PARTS > 1) && `IS_DIVISBLE(DATA_SIZE, PARTS), ("invalid parameter"))
    `STATIC_ASSERT ((ATTR_WIDTH == DONE_WIDTH + (PARTS - 1) * ADDR_WIDTH), ("invalid parameter"))

    localparam PART_SIZE  = DATA_SIZE / PARTS;
    localparam PART_WIDTH = PART_SIZE * 8;
    localparam LOG_NUM_REQS = `ARB_SEL_BITS(NUM_INPUTS, 1);
    localparam SEL_W      = `UP(LOG_NUM_REQS);
    localparam OUT_TAG_W  = TAG_WIDTH + LOG_NUM_REQS;
    localparam TAG_VALUE_W = TAG_WIDTH - UUID_WIDTH;
    localparam REQ_DATAW  = ADDR_WIDTH + DATA_SIZE * 8 + DATA_SIZE + ATTR_WIDTH + OUT_TAG_W;

    wire [NUM_INPUTS-1:0]                    valid_in;
    wire [NUM_INPUTS-1:0][PARTS-1:0]         part_mask;
    wire [NUM_INPUTS-1:0]                    last_in;
    wire [NUM_INPUTS-1:0][TAG_VALUE_W-1:0]   tag_value_in;

    for (genvar i = 0; i < NUM_INPUTS; ++i) begin : g_in
        assign valid_in[i] = bus_in_if[i].req_valid;
        for (genvar p = 0; p < PARTS; ++p) begin : g_part
            assign part_mask[i][p] = (| bus_in_if[i].req_data.byteen[p * PART_SIZE +: PART_SIZE]);
        end
        assign last_in[i]      = bus_in_if[i].req_data.attr[DONE_WIDTH-1];
        assign tag_value_in[i] = bus_in_if[i].req_data.tag.value;
        `UNUSED_VAR (bus_in_if[i].req_data.rw)
        if (ATTR_WIDTH > DONE_WIDTH) begin : g_attr_hi
            `UNUSED_VAR (bus_in_if[i].req_data.attr[ATTR_WIDTH-1:DONE_WIDTH])
        end
    end

    // lead beat

    wire                  lead_valid;
    wire [SEL_W-1:0]      lead_idx;
    wire [NUM_INPUTS-1:0] lead_onehot;
    wire                  out_ready;

    VX_generic_arbiter #(
        .NUM_REQS (NUM_INPUTS),
        .TYPE     ("R")
    ) lead_arb (
        .clk          (clk),
        .reset        (reset),
        .requests     (valid_in),
        .grant_valid  (lead_valid),
        .grant_index  (lead_idx),
        .grant_onehot (lead_onehot),
        .grant_ready  (out_ready)
    );

    wire [PARTS-1:0]       lead_mask;
    wire                   lead_last;
    wire [TAG_VALUE_W-1:0] lead_tag_value;
    wire [NUM_INPUTS-1:0][PARTS + 1 + TAG_VALUE_W - 1:0] lead_info_in;
    for (genvar i = 0; i < NUM_INPUTS; ++i) begin : g_lead_info_in
        assign lead_info_in[i] = {part_mask[i], last_in[i], tag_value_in[i]};
    end
    VX_onehot_mux #(
        .DATAW (PARTS + 1 + TAG_VALUE_W),
        .N     (NUM_INPUTS)
    ) lead_info_mux (
        .data_in  (lead_info_in),
        .sel_in   (lead_onehot),
        .data_out ({lead_mask, lead_last, lead_tag_value})
    );

    // partners: the first worker after the lead, per part the lead leaves empty

    // rotation that puts the input after the lead at position 0
    wire [SEL_W:0] rot_amt = (SEL_W+1)'(lead_idx) + (SEL_W+1)'(1);

    wire [PARTS-1:0]                  has_partner;
    wire [PARTS-1:0][NUM_INPUTS-1:0]  part_owner;

    for (genvar p = 0; p < PARTS; ++p) begin : g_partner
        wire [NUM_INPUTS-1:0] cand;
        for (genvar i = 0; i < NUM_INPUTS; ++i) begin : g_cand
            assign cand[i] = valid_in[i]
                          && (part_mask[i] == PARTS'(1 << p))
                          && ~last_in[i]
                          && (tag_value_in[i] == lead_tag_value);
        end
        wire [2*NUM_INPUTS-1:0] cand2 = {cand, cand};
        wire [NUM_INPUTS-1:0]   cand_rot = NUM_INPUTS'(cand2 >> rot_amt);
        wire [NUM_INPUTS-1:0]   pick_rot;
        wire                    pick_valid;
        VX_priority_encoder #(
            .N (NUM_INPUTS)
        ) pick_pe (
            .data_in    (cand_rot),
            .onehot_out (pick_rot),
            `UNUSED_PIN (index_out),
            .valid_out  (pick_valid)
        );
        wire [NUM_INPUTS-1:0]   pick = NUM_INPUTS'(({pick_rot, pick_rot} << rot_amt) >> NUM_INPUTS);

        assign has_partner[p] = lead_valid && ~lead_mask[p] && ~lead_last && pick_valid;
        assign part_owner[p]  = has_partner[p] ? pick : lead_onehot;
    end

    // granted beats: the lead and the partners
    reg [NUM_INPUTS-1:0] grant_onehot;
    always @(*) begin
        grant_onehot = lead_onehot;
        for (integer p = 0; p < PARTS; ++p) begin
            if (has_partner[p]) begin
                grant_onehot = grant_onehot | part_owner[p];
            end
        end
    end

    for (genvar i = 0; i < NUM_INPUTS; ++i) begin : g_ready
        assign bus_in_if[i].req_ready = out_ready && grant_onehot[i];
    end

    // the merged write

    wire [PARTS-1:0][ADDR_WIDTH-1:0]  part_addr;
    wire [PARTS-1:0][PART_WIDTH-1:0]  part_data;
    wire [PARTS-1:0][PART_SIZE-1:0]   part_byteen;

    for (genvar p = 0; p < PARTS; ++p) begin : g_part_mux
        wire [NUM_INPUTS-1:0][ADDR_WIDTH + PART_WIDTH + PART_SIZE - 1:0] part_in;
        for (genvar i = 0; i < NUM_INPUTS; ++i) begin : g_part_in
            assign part_in[i] = {bus_in_if[i].req_data.addr,
                                 bus_in_if[i].req_data.data[p * PART_WIDTH +: PART_WIDTH],
                                 bus_in_if[i].req_data.byteen[p * PART_SIZE +: PART_SIZE]};
        end
        VX_onehot_mux #(
            .DATAW (ADDR_WIDTH + PART_WIDTH + PART_SIZE),
            .N     (NUM_INPUTS)
        ) part_mux (
            .data_in  (part_in),
            .sel_in   (part_owner[p]),
            .data_out ({part_addr[p], part_data[p], part_byteen[p]})
        );
    end

    wire [DONE_WIDTH-1:0] lead_done;
    wire [TAG_WIDTH-1:0]  lead_tag;
    wire [NUM_INPUTS-1:0][DONE_WIDTH + TAG_WIDTH - 1:0] lead_in;
    for (genvar i = 0; i < NUM_INPUTS; ++i) begin : g_lead_in
        assign lead_in[i] = {bus_in_if[i].req_data.attr[DONE_WIDTH-1:0], bus_in_if[i].req_data.tag};
    end
    VX_onehot_mux #(
        .DATAW (DONE_WIDTH + TAG_WIDTH),
        .N     (NUM_INPUTS)
    ) lead_mux (
        .data_in  (lead_in),
        .sel_in   (lead_onehot),
        .data_out ({lead_done, lead_tag})
    );

    wire [OUT_TAG_W-1:0] out_tag;
    if (LOG_NUM_REQS != 0) begin : g_tag_sel
        VX_bits_insert #(
            .N   (TAG_WIDTH),
            .S   (LOG_NUM_REQS),
            .POS (TAG_SEL_IDX)
        ) bits_insert (
            .data_in  (lead_tag),
            .ins_in   (lead_idx),
            .data_out (out_tag)
        );
    end else begin : g_tag
        `UNUSED_VAR (lead_idx)
        assign out_tag = lead_tag;
    end

    wire [ATTR_WIDTH-1:0] out_attr;
    for (genvar p = 1; p < PARTS; ++p) begin : g_attr_addr
        assign out_attr[DONE_WIDTH + (p - 1) * ADDR_WIDTH +: ADDR_WIDTH] = part_addr[p];
    end
    assign out_attr[DONE_WIDTH-1:0] = lead_done;

    wire [REQ_DATAW-1:0] req_data_out;

    VX_elastic_buffer #(
        .DATAW   (REQ_DATAW),
        .SIZE    (`TO_OUT_BUF_SIZE(REQ_OUT_BUF)),
        .OUT_REG (`TO_OUT_BUF_REG(REQ_OUT_BUF)),
        .LUTRAM  (`TO_OUT_BUF_LUTRAM(REQ_OUT_BUF))
    ) out_buf (
        .clk       (clk),
        .reset     (reset),
        .valid_in  (lead_valid),
        .ready_in  (out_ready),
        .data_in   ({part_addr[0], part_data, part_byteen, out_attr, out_tag}),
        .data_out  (req_data_out),
        .valid_out (bus_out_if[0].req_valid),
        .ready_out (bus_out_if[0].req_ready)
    );

    assign bus_out_if[0].req_data.rw = 1'b1;
    assign {
        bus_out_if[0].req_data.addr,
        bus_out_if[0].req_data.data,
        bus_out_if[0].req_data.byteen,
        bus_out_if[0].req_data.attr,
        bus_out_if[0].req_data.tag
    } = req_data_out;

    // writes only: no response
    assign bus_out_if[0].rsp_ready = 1'b0;
    `UNUSED_VAR (bus_out_if[0].rsp_valid)
    `UNUSED_VAR (bus_out_if[0].rsp_data)
    for (genvar i = 0; i < NUM_INPUTS; ++i) begin : g_rsp
        assign bus_in_if[i].rsp_valid = 1'b0;
        assign bus_in_if[i].rsp_data  = '0;
        `UNUSED_VAR (bus_in_if[i].rsp_ready)
    end

endmodule
