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

// VX_kmu_bus_arb — merges launch masters onto the device launch stream and fans
// it back out across the hierarchy (device -> clusters -> sockets -> cores).
//
// The unit of arbitration is a MESSAGE, not a beat (see VX_kmu_bus_if). Both
// directions therefore LOCK an input->output pair at the header and hold it to
// `eop`, so a multi-beat fragment launch cannot be interleaved with another
// master's header, nor sprayed across two cores.
//
// The lock keys on `eop`, deliberately NOT on `valid` falling. A stream arbiter's
// usual "sticky until the request drops" rule is wrong on this bus: VX_kmu holds
// `valid` high continuously while it has CTAs left to issue, so a valid-keyed
// lock would never release and would starve every other master forever.
//
// Fan-out picks the output by kind:
//   FRAGMENT — routed by `dest`, this level's digit (dest / DEST_DIV) % NUM_OUTPUTS.
//              Fragment work is bin->core affine; load-balancing it would break
//              same-pixel blend order.
//   COMPUTE  — round-robin over the ready outputs, which is what the plain
//              stream arbiter did before and what keeps CTAs spread across cores.
//              An output with no enabled core below it (`out_cores` == 0, every
//              core masked off for launches) takes no new compute message. When
//              the enabled outputs hold different numbers of enabled cores, the
//              round-robin is weighted so that each enabled core, not each
//              subtree, gets an equal share: every output has a credit counter,
//              a new message goes to the nearest ready, enabled output from
//              rr_ptr that still has credit and takes one, and when no ready,
//              enabled output has credit left all counters reload to
//              `out_cores` and the message goes to the nearest ready, enabled
//              output in the same cycle. With equal counts the plain
//              round-robin runs and the counters are not used. The counters
//              clear when the counts change, and otherwise run on across
//              launches, as the round-robin pointer does.

module VX_kmu_bus_arb import VX_gpu_pkg::*; #(
    parameter NUM_INPUTS  = 1,
    parameter NUM_OUTPUTS = 1,
    parameter DEST_DIV    = 1,   // cores below each output: this level's digit of `dest`
    parameter OUT_BUF     = 0,
    parameter `STRING ARBITER = "R"
) (
    input wire              clk,
    input wire              reset,

    // Stateless elastic launch router: pure fan-in/fan-out, no liveness side-channel.
    // A launch beat resident in an output skid is observed by each hierarchy level
    // folding the launch-link `valid` into its own busy (see VX_socket/VX_cluster/
    // Vortex/VX_graphics), so the transport itself carries no busy or occupancy state.
    VX_kmu_bus_if.slave  bus_in_if [NUM_INPUTS],
    VX_kmu_bus_if.master bus_out_if [NUM_OUTPUTS],

    // Enabled cores below each output. Static for a launch: the core mask is
    // written between launches through the KMU's DCRs.
    input wire [NUM_OUTPUTS-1:0][`CLOG2(DEST_DIV+1)-1:0] out_cores
);
    // Single-stage router only: a merge (N->1) or a distribute (1->M), never a
    // crossbar. A genuine N->M would chain the fan-in arbiter and the fan-out mux
    // with no register between them, so the slave-port `ready` would cross both
    // stages combinationally (violating the "register outgoing interface" rule).
    // Compose two instances for N->M so the boundary between them is a registered,
    // busy-visible launch link (see VX_cluster raster merge/distribute).
    `STATIC_ASSERT((NUM_INPUTS == 1) || (NUM_OUTPUTS == 1),
        ("VX_kmu_bus_arb is single-stage (N->1 or 1->M); compose two for N->M"))

    // packed beat: {kind, eop, dest, data}
    localparam PW    = 1 + 1 + KMU_DEST_W + KMU_DATAW;
    localparam LOG_I = `CLOG2(NUM_INPUTS);
    localparam LOG_O = `CLOG2(NUM_OUTPUTS);
    localparam SEL_I = `UP(LOG_I);
    localparam SEL_O = `UP(LOG_O);
    localparam CORES_W = `CLOG2(DEST_DIV+1);

    // field offsets within a packed beat
    localparam PW_DEST = KMU_DATAW;
    localparam PW_EOP  = KMU_DATAW + KMU_DEST_W;
    localparam PW_KIND = KMU_DATAW + KMU_DEST_W + 1;

    wire [NUM_INPUTS-1:0]           bus_valid;
    wire [NUM_INPUTS-1:0][PW-1:0]   bus_data;
    wire [NUM_INPUTS-1:0]           bus_ready;

    wire [NUM_INPUTS-1:0]           in_valid;
    wire [NUM_INPUTS-1:0][PW-1:0]   in_data;
    wire [NUM_INPUTS-1:0]           in_ready;

    for (genvar i = 0; i < NUM_INPUTS; ++i) begin : g_in
        assign bus_valid[i] = bus_in_if[i].valid;
        assign bus_data[i]  = {bus_in_if[i].kind,
                               bus_in_if[i].eop,
                               bus_in_if[i].dest,
                               bus_in_if[i].data};
        assign bus_in_if[i].ready = bus_ready[i];
    end

    // Stateless input: the fan-in grant is presented straight to each master. The
    // grant terminates at the producer's own registered output, so back-pressure
    // never crosses more than one arb between registers -- the skid belongs to the
    // producing endpoint, not this router (a transport module owns no liveness).
    assign in_valid  = bus_valid;
    assign in_data   = bus_data;
    assign bus_ready = in_ready;

    // ── fan-in: NUM_INPUTS masters -> one merged message stream ────────────
    wire            m_valid;
    wire [PW-1:0]   m_data;
    wire            m_ready;

    if (NUM_INPUTS > 1) begin : g_fanin

        // Message lock: once a header has been granted, only that input may
        // drive the merged stream until its `eop` goes through.
        reg             lock_v;
        reg [SEL_I-1:0] lock_sel;

        wire [NUM_INPUTS-1:0] lock_mask =
            lock_v ? (NUM_INPUTS'(1) << lock_sel) : {NUM_INPUTS{1'b1}};

        wire [NUM_INPUTS-1:0] arb_valid_in = in_valid & lock_mask;
        wire [NUM_INPUTS-1:0] arb_ready_in;
        wire [0:0][SEL_I-1:0] arb_sel_out;

        // Ready must be masked too: an ungranted input whose valid we gated off
        // would otherwise see ready high and believe its beat was taken.
        assign in_ready = arb_ready_in & lock_mask;

        VX_stream_arb #(
            .NUM_INPUTS  (NUM_INPUTS),
            .NUM_OUTPUTS (1),
            .DATAW       (PW),
            .ARBITER     (ARBITER),
            .OUT_BUF     (0)
        ) fanin_arb (
            .clk       (clk),
            .reset     (reset),
            .valid_in  (arb_valid_in),
            .ready_in  (arb_ready_in),
            .data_in   (in_data),
            .data_out  (m_data),
            .sel_out   (arb_sel_out),
            .valid_out (m_valid),
            .ready_out (m_ready)
        );

        wire m_fire = m_valid && m_ready;
        always @(posedge clk) begin
            if (reset) begin
                lock_v   <= 1'b0;
                lock_sel <= '0;
            end else if (m_fire) begin
                lock_v   <= ~m_data[PW_EOP];
                lock_sel <= arb_sel_out[0];
            end
        end

    end else begin : g_fanin_1

        assign m_valid    = in_valid[0];
        assign m_data     = in_data[0];
        assign in_ready[0] = m_ready;

    end

    // ── fan-out: one merged stream -> NUM_OUTPUTS ──────────────────────────
    wire [NUM_OUTPUTS-1:0]         out_valid;
    wire [NUM_OUTPUTS-1:0][PW-1:0] out_data;
    wire [NUM_OUTPUTS-1:0]         out_ready;

    if (NUM_OUTPUTS > 1) begin : g_fanout

        reg             lock_v;
        reg [SEL_O-1:0] lock_sel;
        reg [SEL_O-1:0] rr_ptr;
        reg [NUM_OUTPUTS-1:0][CORES_W-1:0] credit;

        wire [SEL_O-1:0] dest_sel = SEL_O'((32'(m_data[PW_DEST +: KMU_DEST_W]) / DEST_DIV) % NUM_OUTPUTS);

        reg weighted;
        always @(*) begin
            weighted = 1'b0;
            for (integer i = 0; i < NUM_OUTPUTS; ++i) begin
                for (integer j = i + 1; j < NUM_OUTPUTS; ++j) begin
                    if ((out_cores[i] != '0) && (out_cores[j] != '0) && (out_cores[i] != out_cores[j])) begin
                        weighted = 1'b1;
                    end
                end
            end
        end

        // The counts and the weighted select are registered to keep the
        // popcounts and comparisons out of the ready path. They change only on
        // a mask DCR write, which precedes the launch it applies to by many
        // cycles, so no CTA sees the cycle of lag.
        reg [NUM_OUTPUTS-1:0][CORES_W-1:0] cores_r;
        reg weighted_r;
        always @(posedge clk) begin
            if (reset) begin
                cores_r    <= '0;
                weighted_r <= 1'b0;
            end else begin
                cores_r    <= out_cores;
                weighted_r <= weighted;
            end
        end
        wire cores_changed = (out_cores != cores_r);

        wire [NUM_OUTPUTS-1:0] out_enabled;
        wire [NUM_OUTPUTS-1:0] has_credit;
        for (genvar o = 0; o < NUM_OUTPUTS; ++o) begin : g_enabled
            assign out_enabled[o] = (cores_r[o] != '0);
            assign has_credit[o]  = (credit[o] != '0);
        end

        wire [NUM_OUTPUTS-1:0] elig        = out_ready & out_enabled;
        wire [NUM_OUTPUTS-1:0] elig_credit = elig & has_credit;
        wire use_credit = weighted_r && (elig_credit != '0);

        // Round-robin over the ready, enabled outputs (with credit, when
        // weighted), starting at rr_ptr. Descending scan so the nearest one
        // (smallest step from rr_ptr) is the last assignment and therefore wins.
        // Both scans run in parallel so the credit test is not in series.
        reg [SEL_O-1:0] rr_sel_all, rr_sel_credit;
        always @(*) begin
            rr_sel_all    = rr_ptr;
            rr_sel_credit = rr_ptr;
            for (integer k = NUM_OUTPUTS - 1; k >= 0; --k) begin
                automatic logic [SEL_O-1:0] idx =
                    SEL_O'((32'(rr_ptr) + k) % NUM_OUTPUTS);
                if (elig[idx]) begin
                    rr_sel_all = idx;
                end
                if (elig_credit[idx]) begin
                    rr_sel_credit = idx;
                end
            end
        end
        wire [SEL_O-1:0] rr_sel = use_credit ? rr_sel_credit : rr_sel_all;

        wire is_fragment = (m_data[PW_KIND] == KMU_KIND_FRAGMENT);
        wire [SEL_O-1:0] sel_new = is_fragment ? dest_sel : rr_sel;
        wire [SEL_O-1:0] sel = lock_v ? lock_sel : sel_new;
        // With no output eligible rr_sel falls back to rr_ptr, which may be
        // disabled: hold the header rather than send it there.
        wire sel_ok = lock_v || is_fragment || out_enabled[sel];

        for (genvar o = 0; o < NUM_OUTPUTS; ++o) begin : g_sel
            assign out_valid[o] = m_valid && sel_ok && (sel == SEL_O'(o));
            assign out_data[o]  = m_data;
        end
        assign m_ready = out_ready[sel] && sel_ok;

        wire m_fire = m_valid && m_ready;
        wire [SEL_O-1:0] rr_next = (sel == SEL_O'(NUM_OUTPUTS - 1)) ? '0 : (sel + SEL_O'(1));

        // A message takes one credit at its header; the granted output is
        // enabled, so a reload leaves it at least one to take.
        wire credit_fire = m_fire && ~lock_v && ~is_fragment && weighted_r;

        always @(posedge clk) begin
            if (reset) begin
                lock_v   <= 1'b0;
                lock_sel <= '0;
                rr_ptr   <= '0;
                credit   <= '0;
            end else begin
                if (m_fire) begin
                    lock_v   <= ~m_data[PW_EOP];
                    lock_sel <= sel;
                    if (m_data[PW_EOP]) begin
                        rr_ptr <= rr_next;
                    end
                    if (credit_fire) begin
                        for (integer o = 0; o < NUM_OUTPUTS; ++o) begin
                            credit[o] <= (use_credit ? credit[o] : cores_r[o])
                                       - CORES_W'(sel == SEL_O'(o));
                        end
                    end
                end
                // Credits loaded for other counts would start the next launch
                // part-way into a window of the old mask.
                if (cores_changed) begin
                    credit <= '0;
                end
            end
        end

    end else begin : g_fanout_1

        // Registered as at a wider level. The mask is static for a launch, so
        // gating each compute beat gates whole messages.
        reg enabled_r;
        always @(posedge clk) begin
            if (reset) begin
                enabled_r <= 1'b0;
            end else begin
                enabled_r <= (out_cores[0] != '0);
            end
        end
        wire sel_ok = (m_data[PW_KIND] == KMU_KIND_FRAGMENT) || enabled_r;
        assign out_valid[0] = m_valid && sel_ok;
        assign out_data[0]  = m_data;
        assign m_ready      = out_ready[0] && sel_ok;

    end

    // ── output buffering ───────────────────────────────────────────────────
    // A per-output elastic buffer is FIFO, so it preserves the beat order of a
    // locked message.
    for (genvar o = 0; o < NUM_OUTPUTS; ++o) begin : g_out
        wire [PW-1:0] buf_data;

        VX_elastic_buffer #(
            .DATAW   (PW),
            .SIZE    (`TO_OUT_BUF_SIZE(OUT_BUF)),
            .OUT_REG (`TO_OUT_BUF_REG(OUT_BUF)),
            .LUTRAM  (`TO_OUT_BUF_LUTRAM(OUT_BUF))
        ) out_buf (
            .clk       (clk),
            .reset     (reset),
            .valid_in  (out_valid[o]),
            .ready_in  (out_ready[o]),
            .data_in   (out_data[o]),
            .data_out  (buf_data),
            .valid_out (bus_out_if[o].valid),
            .ready_out (bus_out_if[o].ready)
        );

        assign bus_out_if[o].kind = buf_data[PW_KIND];
        assign bus_out_if[o].eop  = buf_data[PW_EOP];
        assign bus_out_if[o].dest = buf_data[PW_DEST +: KMU_DEST_W];
        assign bus_out_if[o].data = buf_data[KMU_DATAW-1:0];
    end

endmodule
