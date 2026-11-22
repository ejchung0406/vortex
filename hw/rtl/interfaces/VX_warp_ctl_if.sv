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

interface VX_warp_ctl_if import VX_gpu_pkg::*; ();

    wire tmc_valid;
    wire split_valid;
    wire sjoin_valid;
    wire wsync_valid;
    wire [NW_WIDTH-1:0] wid;
    tmc_t     tmc;
    split_t   split;
    join_t    sjoin;

    // Pipeline drain status of the scheduler's warps, by SFU-block-local warp id
    wire [SFU_BLOCK_WARPS-1:0] warp_pending_alm_empty;

    // Every LSU block's memory scheduler is empty, and the translation path
    // drained. VX_wctl_unit ANDs them to gate BAR so the barrier acts as a
    // SMEM/GMEM fence — pending LSU memory ops finish before the warp suspends
    // or the barrier arrival is registered. This is the CUDA __syncthreads /
    // OpenCL barrier(CLK_LOCAL_MEM_FENCE) contract. With
    // VX_CFG_BAR_DRAIN_SCOPE=1 a barrier that stalls its warp uses the warp's
    // pending status, lsu_sched_sent and lsu_store_landed instead; the last
    // two are core-wide and see loads as well as stores.
    wire [`VX_CFG_NUM_LSU_BLOCKS-1:0] lsu_sched_empty;
    // the same, but for reads awaiting their response: every request sent
    wire [`VX_CFG_NUM_LSU_BLOCKS-1:0] lsu_sched_sent;
    // no request in flight past the schedulers: the paths to the data cache
    // (through its dMMU lanes and link) and to local memory hold none
    wire lsu_store_landed;
    wire mmu_empty;

    // IPDOM stack address and mscratch of the warp at dvstack_wid
    wire [NW_WIDTH-1:0] dvstack_wid;
    wire [DV_STACK_SIZEW-1:0] dvstack_ptr;
    wire [`VX_CFG_MEM_ADDR_WIDTH-1:0] mscratch;

    modport master (
        output tmc_valid,
        output split_valid,
        output sjoin_valid,
        output wsync_valid,
        output wid,
        output tmc,
        output split,
        output sjoin,
        input  warp_pending_alm_empty,
        input  lsu_sched_empty,
        input  lsu_sched_sent,
        input  lsu_store_landed,
        input  mmu_empty,
        output dvstack_wid,
        input  dvstack_ptr,
        input  mscratch
    );

    modport slave (
        input  tmc_valid,
        input  split_valid,
        input  sjoin_valid,
        input  wsync_valid,
        input  wid,
        input  tmc,
        input  split,
        input  sjoin,
        output warp_pending_alm_empty,
        output lsu_sched_empty,
        output lsu_sched_sent,
        output lsu_store_landed,
        output mmu_empty,
        input  dvstack_wid,
        output dvstack_ptr,
        output mscratch
    );

endinterface
