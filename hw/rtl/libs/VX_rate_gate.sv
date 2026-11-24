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

`include "VX_platform.vh"

// A byte-rate gate over NUM_STREAMS parallel streams. Together the streams
// pass at most RATE bytes of charged entries a cycle, each charged entry
// costing LINE_SIZE bytes; an entry with charge_in low passes free. The credit
// grows by RATE a cycle up to one line per stream beyond it, so the long-run
// rate is exact and what an idle spell saves is at most one line per stream.
// Charged entries take turns round-robin from where the last cycle's grants
// ended. A stream passes at most one entry a cycle, through a registered
// two-entry buffer.
`TRACING_OFF
module VX_rate_gate #(
    parameter NUM_STREAMS = 1,
    parameter DATAW       = 1,
    parameter RATE        = 1,
    parameter LINE_SIZE   = 64
) (
    input  wire                              clk,
    input  wire                              reset,

    input  wire [NUM_STREAMS-1:0]            valid_in,
    input  wire [NUM_STREAMS-1:0][DATAW-1:0] data_in,
    input  wire [NUM_STREAMS-1:0]            charge_in,
    output wire [NUM_STREAMS-1:0]            ready_in,

    output wire [NUM_STREAMS-1:0]            valid_out,
    output wire [NUM_STREAMS-1:0][DATAW-1:0] data_out,
    input  wire [NUM_STREAMS-1:0]            ready_out,

    output wire                              empty
);
    `STATIC_ASSERT(`IS_POW2(LINE_SIZE), ("invalid parameter: LINE_SIZE=%0d must be a power of two", LINE_SIZE))
    `STATIC_ASSERT(RATE > 0, ("invalid parameter: RATE=%0d", RATE))

    localparam LINE_BITS = `CLOG2(LINE_SIZE);
    localparam CAP       = RATE + NUM_STREAMS * LINE_SIZE;
    localparam CREDIT_W  = `CLOG2(CAP + RATE + 1);
    localparam QUOTA_W   = CREDIT_W - LINE_BITS;
    localparam COUNT_W   = `CLOG2(NUM_STREAMS + 1);
    localparam PTR_W     = `LOG2UP(NUM_STREAMS);
    localparam CMP_W     = `MAX(QUOTA_W, COUNT_W);

    reg [CREDIT_W-1:0] credit_r;
    reg [PTR_W-1:0]    ptr_r;

    wire [NUM_STREAMS-1:0] buf_ready;

    wire [CREDIT_W-1:0] credit_sum = credit_r + CREDIT_W'(RATE);
    wire [CREDIT_W-1:0] credit_now = (credit_sum > CREDIT_W'(CAP)) ? CREDIT_W'(CAP) : credit_sum;
    wire [QUOTA_W-1:0]  quota = credit_now[CREDIT_W-1:LINE_BITS];

    // The charged candidates in this cycle's order, the stream at ptr_r first.
    // A candidate passes while fewer than `quota` charged ones are ahead of it,
    // so a stream's ready never depends on its own valid.
    wire [NUM_STREAMS-1:0] charged = valid_in & buf_ready & charge_in;
    reg [NUM_STREAMS-1:0] charged_rot;
    always @(*) begin
        charged_rot = '0;
        for (integer k = 0; k < NUM_STREAMS; ++k) begin
            charged_rot[k] = charged[(32'(ptr_r) + k) % NUM_STREAMS];
        end
    end

    wire [NUM_STREAMS-1:0] room_rot;
    for (genvar k = 0; k < NUM_STREAMS; ++k) begin : g_room
        if (k == 0) begin : g_first
            assign room_rot[k] = (quota != 0);
        end else begin : g_ahead
            wire [`CLOG2(k+1)-1:0] ahead;
            VX_popcount #(
                .N (k)
            ) ahead_count (
                .data_in  (charged_rot[k-1:0]),
                .data_out (ahead)
            );
            assign room_rot[k] = (CMP_W'(ahead) < CMP_W'(quota));
        end
    end

    reg [NUM_STREAMS-1:0] room;
    always @(*) begin
        room = '0;
        for (integer k = 0; k < NUM_STREAMS; ++k) begin
            room[(32'(ptr_r) + k) % NUM_STREAMS] = room_rot[k];
        end
    end

    assign ready_in = buf_ready & (~charge_in | room);

    wire [NUM_STREAMS-1:0] fire_charged_rot = charged_rot & room_rot;
    wire [COUNT_W-1:0] used;
    VX_popcount #(
        .N (NUM_STREAMS)
    ) used_count (
        .data_in  (fire_charged_rot),
        .data_out (used)
    );

    // the turn passes to the stream after the last charged grant
    reg [PTR_W-1:0] last_rot;
    always @(*) begin
        last_rot = '0;
        for (integer k = 0; k < NUM_STREAMS; ++k) begin
            if (fire_charged_rot[k]) begin
                last_rot = PTR_W'(k);
            end
        end
    end
    wire [PTR_W:0] ptr_sum = {1'b0, ptr_r} + {1'b0, last_rot} + (PTR_W+1)'(1);
    wire [PTR_W-1:0] ptr_n = PTR_W'((ptr_sum >= (PTR_W+1)'(NUM_STREAMS)) ? (ptr_sum - (PTR_W+1)'(NUM_STREAMS)) : ptr_sum);

    always @(posedge clk) begin
        if (reset) begin
            // full, as after any idle spell
            credit_r <= CREDIT_W'(CAP - RATE);
            ptr_r    <= '0;
        end else begin
            credit_r <= credit_now - (CREDIT_W'(used) << LINE_BITS);
            if (| fire_charged_rot) begin
                ptr_r <= ptr_n;
            end
        end
    end

    for (genvar s = 0; s < NUM_STREAMS; ++s) begin : g_out_buf
        VX_elastic_buffer #(
            .DATAW   (DATAW),
            .SIZE    (2),
            .OUT_REG (1)
        ) out_buf (
            .clk       (clk),
            .reset     (reset),
            .valid_in  (valid_in[s] && (~charge_in[s] || room[s])),
            .ready_in  (buf_ready[s]),
            .data_in   (data_in[s]),
            .data_out  (data_out[s]),
            .valid_out (valid_out[s]),
            .ready_out (ready_out[s])
        );
    end

    assign empty = ~(| valid_out);

endmodule
`TRACING_ON
