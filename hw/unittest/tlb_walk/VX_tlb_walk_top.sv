`include "VX_define.vh"

module VX_tlb_walk_top import VX_gpu_pkg::*, VX_tlb_pkg::*; (
    input wire clk,
    input wire reset,
    input wire req_valid,
    input wire [7:0] req_id,
    output wire req_ready,
    output wire rsp_valid,
    output wire [7:0] rsp_id,
    output wire rsp_ok,
    input wire walk_ready,
    output wire walk_valid,
    output wire [1:0] walk_slot,
    output wire [TLB_VPN_WIDTH-1:0] walk_vpn,
    input wire fill_valid,
    input wire [1:0] fill_slot,
    input wire [TLB_PPN_WIDTH-1:0] fill_ppn,
    output wire fill_ready,
    output wire empty
);
    VX_tlb_bus_if #(.ID_WIDTH(8)) client();
    VX_tlb_bus_if #(.ID_WIDTH(2)) walker();
    VX_tlb_flush_if flush();

    assign client.req_valid = req_valid;
    assign client.req_data = '{id: req_id, access: TLB_ACC_RD, amo: 1'b0, vpn: TLB_VPN_WIDTH'(req_id)};
    assign req_ready = client.req_ready;
    assign client.rsp_ready = 1'b1;
    assign rsp_valid = client.rsp_valid;
    assign rsp_id = client.rsp_data.id;
    assign rsp_ok = !client.rsp_data.fault && client.rsp_data.level == '0
                 && client.rsp_data.flags == 8'h13
                 && client.rsp_data.ppn == TLB_PPN_WIDTH'(rsp_id) + 1'b1;

    assign walker.req_ready = walk_ready;
    assign walk_valid = walker.req_valid;
    assign walk_slot = walker.req_data.id;
    assign walk_vpn = walker.req_data.vpn;
    `UNUSED_VAR ({walker.req_data.access, walker.req_data.amo})
    assign walker.rsp_valid = fill_valid;
    assign walker.rsp_data = '{id: fill_slot, fault: 1'b0, level: '0, ppn: fill_ppn, flags: 8'h13};
    assign fill_ready = walker.rsp_ready;
    assign flush.req = 1'b0;
    `UNUSED_VAR (flush.done)

    VX_tlb_l2 #(
        .NUM_ENTRIES (16),
        .NUM_WAYS    (2),
        .MEGA_SIZE   (2),
        .MSHR_SIZE   (4),
        .REQR_DEPTH  (2),
        .LATENCY     (2),
        .ID_WIDTH    (8)
    ) dut (
        .clk       (clk),
        .reset     (reset),
        .client_if (client),
        .ptw_if    (walker),
        .flush_if  (flush),
        .empty     (empty)
    );
endmodule
