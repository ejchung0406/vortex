#!/usr/bin/bash
# instr.sh: src_wr_a = worktree + trace prints (measurement only)
set -e
D=/storage/project/r-hkim358-0/echung67/vortex-exp
$D/slurm/wrr/sync.sh a > /dev/null
cd $D/src_wr_a && python3 - <<'PY'
def sub(path, old, new, cnt=1):
    t=open(path).read()
    assert t.count(old)==cnt, (path, old[:70], t.count(old))
    open(path,'w').write(t.replace(old,new))
p='sim/simx/core.cpp'
T='if (trace->wid == 0) fprintf(stderr, "T %llu %s pc=%llx\\n", (unsigned long long)SimPlatform::instance().cycles(), "{}", (unsigned long long)trace->PC);'
sub(p,'''      fetch_latch->push(trace);''','''      fetch_latch->push(trace);
      '''+T.replace('{}','pick'))
sub(p,'''          trace->log_once(false);
          ++ibuf_queued_.at(trace->wid);''','''          trace->log_once(false);
          '''+T.replace('{}','rsp')+'''
          ++ibuf_queued_.at(trace->wid);''')
sub(p,'''        drop = {true, trace->wid, overtook};''','''        drop = {true, trace->wid, overtook};
        if (trace->wid == 0) fprintf(stderr, "T %llu drop pc=%llx sq=%d\\n", (unsigned long long)SimPlatform::instance().cycles(), (unsigned long long)trace->PC, (int)overtook);''')
sub(p,'''          DT(3, simobject_->name() << "-pipeline issue: " << *uop_trace);''','''          DT(3, simobject_->name() << "-pipeline issue: " << *uop_trace);
          '''+T.replace('{}','iss'))
sub('hw/rtl/core/VX_warp_sched.sv','''    // status
    assign sched_if.active ''','''    always @(posedge clk) begin
        if (sched_fire && slice_id == 0 && cand_lid == 0) $display("T %0t pick pc=%0h", $time, to_fullPC(warp_pcs[cand_lid]));
    end

    // status
    assign sched_if.active ''')
sub('hw/rtl/core/VX_decode.sv','''    assign fetch_if.ibuf_pop = decode_if.ibuf_pop;''','''    assign fetch_if.ibuf_pop = decode_if.ibuf_pop;
    always @(posedge clk) begin
        if (fetch_fire && fetch_if.data.wid == 0 && rsp_drop) $display("T %0t drop pc=%0h sq=%0d %s", $time, to_fullPC(fetch_if.data.PC), rsp_squash, INSTANCE_ID);
        if (fetch_fire && fetch_if.data.wid == 0 && ~rsp_drop) $display("T %0t rsp pc=%0h %s", $time, to_fullPC(fetch_if.data.PC), INSTANCE_ID);
    end''')
sub('hw/rtl/core/VX_scoreboard.sv','''    wire issue_fire = valid_out_w && ready_out_w;''','''    wire issue_fire = valid_out_w && ready_out_w;
    always @(posedge clk) begin
        if (issue_fire && arb_index == 0) $display("T %0t iss pc=%0h %s", $time, to_fullPC(staging_if[0].data.PC), INSTANCE_ID);
    end''')
PY
