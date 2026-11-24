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

// Fast PLRU encoder and decoder utility
// Adapted from BaseJump STL: http://bjump.org/data_out.html

module plru_decoder import VX_gpu_pkg::*; #(
    parameter NUM_WAYS      = 1,
    parameter WAY_IDX_BITS  = $clog2(NUM_WAYS),
    parameter WAY_IDX_WIDTH = `UP(WAY_IDX_BITS)
) (
    input wire [WAY_IDX_WIDTH-1:0]    way_idx,
    output wire [`UP(NUM_WAYS-1)-1:0] lru_data,
    output wire [`UP(NUM_WAYS-1)-1:0] lru_mask
);
    if (NUM_WAYS > 1) begin : g_dec
        wire [`UP(NUM_WAYS-1)-1:0] data;
        wire [`UP(NUM_WAYS-1)-1:0] mask;
        for (genvar i = 0; i < NUM_WAYS-1; ++i) begin : g_i
            if (i == 0) begin : g_i_0
                assign mask[i] = 1'b1;
            end else if (i % 2 == 1) begin : g_i_odd
                assign mask[i] = mask[(i-1)/2] & ~way_idx[WAY_IDX_BITS-$clog2(i+2)+1];
            end else begin : g_i_even
                assign mask[i] = mask[(i-2)/2] & way_idx[WAY_IDX_BITS-$clog2(i+2)+1];
            end
            assign data[i] = ~way_idx[WAY_IDX_BITS-$clog2(i+2)];
        end
        assign lru_data = data;
        assign lru_mask = mask;
    end else begin : g_no_dec
        `UNUSED_VAR (way_idx)
        assign lru_data = '0;
        assign lru_mask = '0;
    end

endmodule

module plru_encoder #(
    parameter NUM_WAYS      = 1,
    parameter WAY_IDX_BITS  = $clog2(NUM_WAYS),
    parameter WAY_IDX_WIDTH = `UP(WAY_IDX_BITS)
) (
    input wire [`UP(NUM_WAYS-1)-1:0] lru_in,
    output wire [WAY_IDX_WIDTH-1:0] way_idx
);
    if (NUM_WAYS > 1) begin : g_enc
        wire [WAY_IDX_BITS-1:0] tmp;
        for (genvar i = 0; i < WAY_IDX_BITS; ++i) begin : g_i
            if (i == 0) begin : g_i_0
                assign tmp[WAY_IDX_WIDTH-1] = lru_in[0];
            end else begin : g_i_n
                VX_mux #(
                    .N (2**i)
                ) mux (
                    .data_in  (lru_in[((2**i)-1)+:(2**i)]),
                    .sel_in   (tmp[WAY_IDX_BITS-1-:i]),
                    .data_out (tmp[WAY_IDX_BITS-1-i])
                );
            end
        end
        assign way_idx = tmp;
    end else begin : g_no_enc
        `UNUSED_VAR (lru_in)
        assign way_idx = '0;
    end

endmodule

module VX_cache_repl #(
    parameter CACHE_SIZE = 1024,
    parameter LINE_SIZE  = 64,                // Size of line inside a bank in bytes
    parameter NUM_BANKS  = 1,                 // Number of banks
    parameter NUM_WAYS   = 1,                 // Number of associative ways
    parameter REPL_POLICY = `CS_REPL_FIFO     // replacement policy
) (
    input wire clk,
    input wire reset,
    input wire stall,
    input wire init,
    input wire touch_valid,
    input wire touch_init,
    input wire [`CS_LINE_SEL_BITS-1:0] touch_line,
    input wire [`CS_WAY_SEL_WIDTH-1:0] touch_way,
    input wire repl_valid,
    input wire [`CS_LINE_SEL_BITS-1:0] repl_line,
    input wire [`CS_LINE_SEL_BITS-1:0] repl_line_n, // look-ahead (next) line for sync-BRAM read
    input wire [NUM_WAYS-1:0] way_valid,
    output wire [`CS_WAY_SEL_WIDTH-1:0] repl_way,
    output wire [NUM_WAYS-1:0] repl_way_oh
);
    localparam WAY_SEL_WIDTH = `CS_WAY_SEL_WIDTH;
    // A way count that is not a power of two leaves the top way codes unused.
    localparam WAYS_POW2     = `IS_POW2(NUM_WAYS);
    `UNUSED_VAR (reset)
    `UNUSED_VAR (init)
    `UNUSED_VAR (stall)
    `UNUSED_VAR (touch_valid)
    `UNUSED_VAR (touch_init)
    `UNUSED_VAR (touch_line)
    `UNUSED_VAR (touch_way)
    `UNUSED_VAR (way_valid)

    if (NUM_WAYS > 1) begin : g_enable
        if (REPL_POLICY == `CS_REPL_PLRU) begin : g_plru
            `UNUSED_VAR (repl_valid)

            localparam TREE_WAYS = 1 << WAY_SEL_WIDTH;
            localparam LRU_WIDTH = TREE_WAYS - 1;

            wire [LRU_WIDTH-1:0] plru_rdata;
            wire [LRU_WIDTH-1:0] plru_wdata;
            wire [LRU_WIDTH-1:0] plru_wmask;
            wire [WAY_SEL_WIDTH-1:0] plru_way;
            wire update = touch_valid && ~stall;
            wire [LRU_WIDTH-1:0] update_data = touch_init ? '0 : plru_wdata;
            wire [LRU_WIDTH-1:0] update_mask = touch_init ? '1 : plru_wmask;

            VX_dp_ram #(
                .DATAW (LRU_WIDTH),
                .SIZE  (`CS_LINES_PER_BANK),
                .WRENW (LRU_WIDTH),
                .OUT_REG (1),
                .RDW_MODE ("R")
            ) plru_store (
                .clk   (clk),
                .reset (1'b0),
                .read  (~stall),
                .write (update),
                .wren  (update_mask),
                .waddr (touch_line),
                .raddr (repl_line_n),
                .wdata (update_data),
                .rdata (plru_rdata)
            );

            plru_decoder #(
                .NUM_WAYS (TREE_WAYS)
            ) plru_dec (
                .way_idx  (touch_way),
                .lru_data (plru_wdata),
                .lru_mask (plru_wmask)
            );

            // The read-first array misses a same-set write on the read edge
            // and the older operation still in the touch stage.
            wire same_set;
            wire [LRU_WIDTH-1:0] prior_data, prior_mask;
            wire [LRU_WIDTH-1:0] read_collision = (update && touch_line == repl_line_n) ? update_mask : '0;
            `BUFFER_EX(same_set, (repl_line == repl_line_n), ~stall, 1, 1);
            `BUFFER_EX(prior_data, update_data, ~stall, LRU_WIDTH, 1);
            `BUFFER_EX(prior_mask, read_collision, ~stall, LRU_WIDTH, 1);

            wire [LRU_WIDTH-1:0] current_mask = (touch_valid && same_set) ? update_mask : '0;
            wire [LRU_WIDTH-1:0] prior_state = (plru_rdata & ~prior_mask) | (prior_data & prior_mask);
            wire [LRU_WIDTH-1:0] current_state = (prior_state & ~current_mask) | (update_data & current_mask);
            wire [LRU_WIDTH-1:0] choices;
            for (genvar i = 0; i < LRU_WIDTH; ++i) begin : g_choice
                localparam LEVEL = $clog2(i + 2) - 1;
                localparam LEAVES = TREE_WAYS >> LEVEL;
                localparam FIRST_LEAF = (i - ((1 << LEVEL) - 1)) * LEAVES;
                localparam HAS_RIGHT = FIRST_LEAF + LEAVES / 2 < NUM_WAYS;
                assign choices[i] = HAS_RIGHT && current_state[i];
            end

            plru_encoder #(
                .NUM_WAYS (TREE_WAYS)
            ) plru_enc (
                .lru_in  (choices),
                .way_idx (plru_way)
            );

            wire has_invalid;
            wire [WAY_SEL_WIDTH-1:0] invalid_way;
            wire [NUM_WAYS-1:0] invalid_way_oh;
            VX_priority_encoder #(
                .N (NUM_WAYS)
            ) free_way_enc (
                .data_in    (~way_valid),
                .onehot_out (invalid_way_oh),
                .index_out  (invalid_way),
                .valid_out  (has_invalid)
            );
            assign repl_way = has_invalid ? invalid_way : plru_way;
            assign repl_way_oh = has_invalid ? invalid_way_oh : (NUM_WAYS'(1) << plru_way);

        end else if (REPL_POLICY == `CS_REPL_FIFO) begin : g_fifo
            // Fifo replacement policy

            wire [WAY_SEL_WIDTH-1:0] fifo_rdata;
            wire [WAY_SEL_WIDTH-1:0] fifo_wdata = (WAYS_POW2 || (fifo_rdata != WAY_SEL_WIDTH'(NUM_WAYS-1))) ? (fifo_rdata + 1) : '0;

            // Same per-cycle look-ahead as plru_store/tag store; read-first so
            // the fill's own increment is not bypassed into its victim read.
            VX_dp_ram #(
                .DATAW (WAY_SEL_WIDTH),
                .SIZE  (`CS_LINES_PER_BANK),
                .OUT_REG (1),
                .RDW_MODE ("R")
            ) fifo_store (
                .clk   (clk),
                .reset (1'b0),
                .read  (~stall),
                .write (init || repl_valid),
                .wren  (1'b1),
                .waddr (repl_line),
                .raddr (repl_line_n),
                .wdata (init ? '0 : fifo_wdata),
                .rdata (fifo_rdata)
            );

            assign repl_way = fifo_rdata;
        end else begin : g_random
            // Random replacement policy
            `UNUSED_VAR (repl_line)
            `UNUSED_VAR (repl_line_n)
            localparam STATE_W = 2 * WAY_SEL_WIDTH;
            reg [STATE_W-1:0] rng_state;
            // Maximal-period LFSR over STATE_W bits using xnor of top two bits.
            wire rng_fb = ~(rng_state[STATE_W-1] ^ rng_state[STATE_W-2]);
            always @(posedge clk) begin
                if (reset) begin
                    rng_state <= {STATE_W{1'b1}};
                end else if (repl_valid) begin
                    rng_state <= {rng_state[STATE_W-2:0], rng_fb};
                end
            end
            // XOR-mix LFSR with line index so different lines see different
            // victim sequences for the same RNG state.
            wire [WAY_SEL_WIDTH-1:0] rng_way = rng_state[WAY_SEL_WIDTH-1:0]
                                             ^ rng_state[STATE_W-1 -: WAY_SEL_WIDTH]
                                             ^ repl_line[WAY_SEL_WIDTH-1:0];
            assign repl_way = (WAYS_POW2 || (rng_way < WAY_SEL_WIDTH'(NUM_WAYS))) ? rng_way
                            : (rng_way - WAY_SEL_WIDTH'(NUM_WAYS));
        end
    end else begin : g_disable
        `UNUSED_VAR (clk)
        `UNUSED_VAR (repl_valid)
        `UNUSED_VAR (repl_line)
        `UNUSED_VAR (repl_line_n)
        assign repl_way = 1'b0;
    end

    if ((REPL_POLICY != `CS_REPL_PLRU) || (NUM_WAYS == 1)) begin : g_repl_way_oh
        assign repl_way_oh = NUM_WAYS'(1) << repl_way;
    end

endmodule
