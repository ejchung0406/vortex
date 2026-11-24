// Copyright © 2019-2026
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

// The switch of an SM group: the group's socket memory ports on one side, its
// uplink lanes to the L2 on the other.
//
// A request takes the lane its line address selects, so one address always
// keeps one path, and carries its port in the low bits of its tag; a response
// returns to the port its tag names. Each direction is a crossbar with one
// round-robin arbiter per output and a registered output.
//
// The L2 serves a flush token when it heads the input it arrived on and holds
// every input's cacheable requests meanwhile. With one private link per port
// that orders a token behind the port's earlier requests and ahead of its
// later ones; across address-hashed lanes it does not. So each port counts
// its requests between here and the L2 (deliver_* reports each one entering
// the L2): a token waits until the port has none left in flight, and the
// port's later requests wait until the token has reached the L2.
module VX_noc_switch import VX_gpu_pkg::*; #(
    parameter NUM_PORTS  = 1,
    parameter NUM_LANES  = 1,
    parameter DATA_SIZE  = 1,
    parameter TAG_WIDTH  = 1, // at the ports; the lanes carry PORT_BITS more
    parameter ADDR_WIDTH = (`VX_CFG_MEM_ADDR_WIDTH - `CLOG2(DATA_SIZE)),
    parameter ATTR_WIDTH = MEM_ATTR_WIDTH,
    parameter PORT_BITS  = `CLOG2(NUM_PORTS)
) (
    input wire              clk,
    input wire              reset,

    VX_mem_bus_if.slave     port_bus_if [NUM_PORTS],
    VX_mem_bus_if.master    lane_bus_if [NUM_LANES],

    input wire [NUM_LANES-1:0]                      deliver_valid,
    input wire [NUM_LANES-1:0][`UP(PORT_BITS)-1:0]  deliver_port,
    input wire [NUM_LANES-1:0]                      deliver_token,

    // nothing of this group between its ports and the L2, either way
    output wire             req_empty,
    output wire             rsp_empty
);
    `UNUSED_VAR (deliver_port)
    `UNUSED_VAR (deliver_token)
    `STATIC_ASSERT(`IS_POW2(NUM_LANES), ("invalid parameter: NUM_LANES=%0d must be a power of two", NUM_LANES))

    localparam DATA_WIDTH = (8 * DATA_SIZE);
    localparam REQ_DATAW  = 1 + ADDR_WIDTH + DATA_WIDTH + DATA_SIZE + ATTR_WIDTH + TAG_WIDTH;
    localparam RSP_DATAW  = DATA_WIDTH + TAG_WIDTH;
    localparam LANE_BITS  = `CLOG2(NUM_LANES);
    localparam PORT_SELW  = `LOG2UP(NUM_PORTS);
    localparam LANE_SELW  = `LOG2UP(NUM_LANES);
    localparam DELIVERW   = `CLOG2(NUM_LANES + 1);
    // a port's requests in flight: bounded by what the lanes can hold
    localparam FLIGHTW    = 16;

    ///////////////////////////////////////////////////////////////////////////

    wire [NUM_PORTS-1:0]                 req_valid_in;
    wire [NUM_PORTS-1:0][REQ_DATAW-1:0]  req_data_in;
    wire [NUM_PORTS-1:0][LANE_SELW-1:0]  req_sel_in;
    wire [NUM_PORTS-1:0]                 req_ready_in;

    wire [NUM_LANES-1:0]                 req_valid_out;
    wire [NUM_LANES-1:0][REQ_DATAW-1:0]  req_data_out;
    wire [NUM_LANES-1:0][PORT_SELW-1:0]  req_sel_out;
    wire [NUM_LANES-1:0]                 req_ready_out;

    wire [NUM_PORTS-1:0] req_fire_in;
    wire [NUM_PORTS-1:0] port_req_idle;

    for (genvar p = 0; p < NUM_PORTS; ++p) begin : g_req_in

        // XOR fold of the line address onto the lanes
        wire [LANE_SELW-1:0] lane_sel;
        if (LANE_BITS != 0) begin : g_fold
            localparam CHUNKS = `CDIV(ADDR_WIDTH, LANE_BITS);
            wire [CHUNKS*LANE_BITS-1:0] addr_x = (CHUNKS*LANE_BITS)'(port_bus_if[p].req_data.addr);
            reg [LANE_BITS-1:0] fold;
            always @(*) begin
                fold = '0;
                for (integer c = 0; c < CHUNKS; ++c) begin
                    fold = fold ^ addr_x[c * LANE_BITS +: LANE_BITS];
                end
            end
            assign lane_sel = fold;
        end else begin : g_one_lane
            assign lane_sel = '0;
        end

        reg [FLIGHTW-1:0] in_flight_r;
        reg               token_out_r;

        wire is_token = port_bus_if[p].req_data.attr[MEM_ATTR_FLUSH_OFFS];
        wire hold = token_out_r || (is_token && (in_flight_r != 0));

        assign req_valid_in[p] = port_bus_if[p].req_valid && ~hold;
        assign req_data_in[p]  = port_bus_if[p].req_data;
        assign req_sel_in[p]   = lane_sel;
        assign port_bus_if[p].req_ready = req_ready_in[p] && ~hold;
        assign req_fire_in[p]  = req_valid_in[p] && req_ready_in[p];

        // a single port owns every delivery: its tag has no port bits
        wire [NUM_LANES-1:0] delivered_here;
        for (genvar l = 0; l < NUM_LANES; ++l) begin : g_delivered
            if (PORT_BITS != 0) begin : g_match
                assign delivered_here[l] = deliver_valid[l] && (deliver_port[l] == PORT_BITS'(p));
            end else begin : g_one_port
                assign delivered_here[l] = deliver_valid[l];
            end
        end
        wire [DELIVERW-1:0] delivered_cnt;
        `POP_COUNT(delivered_cnt, delivered_here);

        wire [FLIGHTW-1:0] in_flight_n = in_flight_r + FLIGHTW'(req_fire_in[p]) - FLIGHTW'(delivered_cnt);

        always @(posedge clk) begin
            if (reset) begin
                in_flight_r <= '0;
                token_out_r <= 1'b0;
            end else begin
                in_flight_r <= in_flight_n;
                if (req_fire_in[p]) begin
                    token_out_r <= is_token;
                end else if (in_flight_n == 0) begin
                    token_out_r <= 1'b0;
                end
            end
        end

        `RUNTIME_ASSERT(~(| delivered_cnt) || (in_flight_r >= FLIGHTW'(delivered_cnt)),
            ("%t: port %0d: a delivery the port did not send", $time, p))
        `RUNTIME_ASSERT(~req_fire_in[p] || (in_flight_r != {FLIGHTW{1'b1}}),
            ("%t: port %0d: more requests in flight than the counter holds", $time, p))
        `RUNTIME_ASSERT(~(| (delivered_here & deliver_token)) || (in_flight_r == FLIGHTW'(1)),
            ("%t: port %0d: a token entered the L2 with other requests of its port in flight", $time, p))

        assign port_req_idle[p] = (in_flight_r == 0);
    end

    assign req_empty = (& port_req_idle);

    VX_stream_xbar #(
        .NUM_INPUTS  (NUM_PORTS),
        .NUM_OUTPUTS (NUM_LANES),
        .DATAW       (REQ_DATAW),
        .ARBITER     ("R"),
        .OUT_BUF     (3)
    ) req_xbar (
        .clk       (clk),
        .reset     (reset),
        .valid_in  (req_valid_in),
        .data_in   (req_data_in),
        .sel_in    (req_sel_in),
        .ready_in  (req_ready_in),
        .valid_out (req_valid_out),
        .data_out  (req_data_out),
        .sel_out   (req_sel_out),
        .ready_out (req_ready_out),
        `UNUSED_PIN (collisions)
    );

    for (genvar l = 0; l < NUM_LANES; ++l) begin : g_req_out
        wire [TAG_WIDTH-1:0] req_tag_out;
        assign lane_bus_if[l].req_valid = req_valid_out[l];
        assign {
            lane_bus_if[l].req_data.rw,
            lane_bus_if[l].req_data.addr,
            lane_bus_if[l].req_data.data,
            lane_bus_if[l].req_data.byteen,
            lane_bus_if[l].req_data.attr,
            req_tag_out
        } = req_data_out[l];
        assign req_ready_out[l] = lane_bus_if[l].req_ready;

        VX_bits_insert #(
            .N   (TAG_WIDTH),
            .S   (PORT_BITS),
            .POS (0)
        ) port_insert (
            .data_in  (req_tag_out),
            .ins_in   (`UP(PORT_BITS)'(req_sel_out[l])),
            .data_out (lane_bus_if[l].req_data.tag)
        );
    end

    ///////////////////////////////////////////////////////////////////////////

    wire [NUM_LANES-1:0]                 rsp_valid_in;
    wire [NUM_LANES-1:0][RSP_DATAW-1:0]  rsp_data_in;
    wire [NUM_LANES-1:0][PORT_SELW-1:0]  rsp_sel_in;
    wire [NUM_LANES-1:0]                 rsp_ready_in;

    wire [NUM_PORTS-1:0]                 rsp_valid_out;
    wire [NUM_PORTS-1:0][RSP_DATAW-1:0]  rsp_data_out;
    wire [NUM_PORTS-1:0]                 rsp_ready_out;

    for (genvar l = 0; l < NUM_LANES; ++l) begin : g_rsp_in
        wire [TAG_WIDTH-1:0]      rsp_tag_in;
        wire [`UP(PORT_BITS)-1:0] rsp_port;
        VX_bits_remove #(
            .N   (TAG_WIDTH + PORT_BITS),
            .S   (PORT_BITS),
            .POS (0)
        ) port_remove (
            .data_in  (lane_bus_if[l].rsp_data.tag),
            .sel_out  (rsp_port),
            .data_out (rsp_tag_in)
        );
        assign rsp_valid_in[l] = lane_bus_if[l].rsp_valid;
        assign rsp_data_in[l]  = {lane_bus_if[l].rsp_data.data, rsp_tag_in};
        assign rsp_sel_in[l]   = PORT_SELW'(rsp_port);
        assign lane_bus_if[l].rsp_ready = rsp_ready_in[l];
    end

    VX_stream_xbar #(
        .NUM_INPUTS  (NUM_LANES),
        .NUM_OUTPUTS (NUM_PORTS),
        .DATAW       (RSP_DATAW),
        .ARBITER     ("R"),
        .OUT_BUF     (3)
    ) rsp_xbar (
        .clk       (clk),
        .reset     (reset),
        .valid_in  (rsp_valid_in),
        .data_in   (rsp_data_in),
        .sel_in    (rsp_sel_in),
        .ready_in  (rsp_ready_in),
        .valid_out (rsp_valid_out),
        .data_out  (rsp_data_out),
        `UNUSED_PIN (sel_out),
        .ready_out (rsp_ready_out),
        `UNUSED_PIN (collisions)
    );

    for (genvar p = 0; p < NUM_PORTS; ++p) begin : g_rsp_out
        assign port_bus_if[p].rsp_valid = rsp_valid_out[p];
        assign port_bus_if[p].rsp_data  = rsp_data_out[p];
        assign rsp_ready_out[p] = port_bus_if[p].rsp_ready;
    end

    function automatic integer xbar_buffers_per_output(integer inputs);
        integer buffers, slices;
        buffers = 1;
        slices = inputs;
        while (slices > (`MAX_FANOUT + `MAX_FANOUT / 2)) begin
            slices = `CDIV(slices, `MAX_FANOUT);
            buffers += slices;
        end
        return buffers;
    endfunction

    // Responses can occupy any of the two-entry buffers of the join tree.
    wire [NUM_LANES-1:0] rsp_fire_in  = rsp_valid_in & rsp_ready_in;
    wire [NUM_PORTS-1:0] rsp_fire_out = rsp_valid_out & rsp_ready_out;

    VX_mem_req_pending #(
        .NUM_INPUTS  (NUM_LANES),
        .NUM_OUTPUTS (NUM_PORTS),
        .SIZE        (`MAX(NUM_LANES, 2 * NUM_PORTS * xbar_buffers_per_output(NUM_LANES)))
    ) rsp_pending (
        .clk      (clk),
        .reset    (reset),
        .in_fire  (rsp_fire_in),
        .out_fire (rsp_fire_out),
        .empty    (rsp_empty)
    );

endmodule
