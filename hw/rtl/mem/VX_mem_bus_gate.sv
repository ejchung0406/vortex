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

// A byte-rate limit over NUM_PORTS memory buses: together they pass at most
// RATE bytes of read lines a cycle, a line being one bus word. With COUNT_REQ
// it counts read requests (writes and flush tokens pass free) and leaves the
// responses alone; without, it counts responses and leaves the requests
// alone. The counted direction takes a cycle more.
module VX_mem_bus_gate import VX_gpu_pkg::*; #(
    parameter NUM_PORTS  = 1,
    parameter DATA_SIZE  = 1,
    parameter TAG_WIDTH  = 1,
    parameter RATE       = 1,
    parameter COUNT_REQ  = 1,
    parameter ADDR_WIDTH = (`VX_CFG_MEM_ADDR_WIDTH - `CLOG2(DATA_SIZE)),
    parameter ATTR_WIDTH = MEM_ATTR_WIDTH
) (
    input wire              clk,
    input wire              reset,
    VX_mem_bus_if.slave     bus_in_if [NUM_PORTS],
    VX_mem_bus_if.master    bus_out_if [NUM_PORTS],
    output wire             empty
);
    localparam DATA_WIDTH = (8 * DATA_SIZE);
    localparam REQ_DATAW  = 1 + ADDR_WIDTH + DATA_WIDTH + DATA_SIZE + ATTR_WIDTH + TAG_WIDTH;
    localparam RSP_DATAW  = DATA_WIDTH + TAG_WIDTH;

    if (COUNT_REQ != 0) begin : g_req

        wire [NUM_PORTS-1:0]                valid_in, ready_in, charge_in;
        wire [NUM_PORTS-1:0][REQ_DATAW-1:0] data_in;
        wire [NUM_PORTS-1:0]                valid_out, ready_out;
        wire [NUM_PORTS-1:0][REQ_DATAW-1:0] data_out;

        for (genvar i = 0; i < NUM_PORTS; ++i) begin : g_ports
            assign valid_in[i]  = bus_in_if[i].req_valid;
            assign data_in[i]   = bus_in_if[i].req_data;
            assign charge_in[i] = ~bus_in_if[i].req_data.rw
                               && ~bus_in_if[i].req_data.attr[MEM_ATTR_FLUSH_OFFS];
            assign bus_in_if[i].req_ready = ready_in[i];

            assign bus_out_if[i].req_valid = valid_out[i];
            assign bus_out_if[i].req_data  = data_out[i];
            assign ready_out[i] = bus_out_if[i].req_ready;

            assign bus_in_if[i].rsp_valid = bus_out_if[i].rsp_valid;
            assign bus_in_if[i].rsp_data  = bus_out_if[i].rsp_data;
            assign bus_out_if[i].rsp_ready = bus_in_if[i].rsp_ready;
        end

        VX_rate_gate #(
            .NUM_STREAMS (NUM_PORTS),
            .DATAW       (REQ_DATAW),
            .RATE        (RATE),
            .LINE_SIZE   (DATA_SIZE)
        ) gate (
            .clk       (clk),
            .reset     (reset),
            .valid_in  (valid_in),
            .data_in   (data_in),
            .charge_in (charge_in),
            .ready_in  (ready_in),
            .valid_out (valid_out),
            .data_out  (data_out),
            .ready_out (ready_out),
            .empty     (empty)
        );

    end else begin : g_rsp

        wire [NUM_PORTS-1:0]                valid_in, ready_in;
        wire [NUM_PORTS-1:0][RSP_DATAW-1:0] data_in;
        wire [NUM_PORTS-1:0]                valid_out, ready_out;
        wire [NUM_PORTS-1:0][RSP_DATAW-1:0] data_out;

        for (genvar i = 0; i < NUM_PORTS; ++i) begin : g_ports
            assign valid_in[i] = bus_out_if[i].rsp_valid;
            assign data_in[i]  = bus_out_if[i].rsp_data;
            assign bus_out_if[i].rsp_ready = ready_in[i];

            assign bus_in_if[i].rsp_valid = valid_out[i];
            assign bus_in_if[i].rsp_data  = data_out[i];
            assign ready_out[i] = bus_in_if[i].rsp_ready;

            assign bus_out_if[i].req_valid = bus_in_if[i].req_valid;
            assign bus_out_if[i].req_data  = bus_in_if[i].req_data;
            assign bus_in_if[i].req_ready  = bus_out_if[i].req_ready;
        end

        VX_rate_gate #(
            .NUM_STREAMS (NUM_PORTS),
            .DATAW       (RSP_DATAW),
            .RATE        (RATE),
            .LINE_SIZE   (DATA_SIZE)
        ) gate (
            .clk       (clk),
            .reset     (reset),
            .valid_in  (valid_in),
            .data_in   (data_in),
            .charge_in ({NUM_PORTS{1'b1}}),
            .ready_in  (ready_in),
            .valid_out (valid_out),
            .data_out  (data_out),
            .ready_out (ready_out),
            .empty     (empty)
        );

    end

endmodule
