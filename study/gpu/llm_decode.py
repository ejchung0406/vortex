#!/usr/bin/env python
"""Steady-state greedy decode of Qwen3.5-9B (language model only, BF16) with a
KV cache, short prompt, B identical concurrent sequences.

A decode step is one forward of B new tokens against the cache. Each step is
timed with torch.cuda.synchronize(); a sequence is restarted from the prompt
after --new-tokens steps so the context stays short.

modes
  solo     per batch size: median ms per step over --reps sequences, tokens/s,
           and model.generate() over the same number of tokens as a cross-check
  window   per batch size, steps run continuously for --duration seconds, then 32
           steps are profiled for the time the GPU spent in kernels. With
           --sync-dir two processes measure the same windows, one per batch size
  profile  torch.profiler CUDA time by operator over --new-tokens decode steps
"""
import argparse, os, statistics, time
import torch

p = argparse.ArgumentParser()
p.add_argument("--mode", default="solo", choices=["solo", "window", "profile"])
p.add_argument("--model", default="Qwen/Qwen3.5-9B")
p.add_argument("--batches", default="1,4,16")
p.add_argument("--new-tokens", type=int, default=128)
p.add_argument("--reps", type=int, default=5)
p.add_argument("--duration", type=float, default=30)
p.add_argument("--sync-dir", help="co-run: directory of ready/go/done files shared with the partner")
p.add_argument("--role", default="b", choices=["a", "b"])
p.add_argument("--tag", default="")
a = p.parse_args()

import transformers
from transformers import AutoModelForCausalLM, AutoTokenizer

dev = "cuda"
sync = torch.cuda.synchronize
tok = AutoTokenizer.from_pretrained(a.model)
# AutoModelForCausalLM maps this checkpoint to the text-only class: no vision tower is built
model = AutoModelForCausalLM.from_pretrained(a.model, dtype=torch.bfloat16).to(dev).eval()
allbytes = sum(q.numel() * q.element_size() for q in model.parameters())
nparam = sum(q.numel() for q in model.parameters())
emb = model.get_input_embeddings().weight
tied = model.get_output_embeddings().weight is emb
# weights one decode step reads: every matrix except the input embedding table, of which a
# step gathers B rows (when the table is tied to lm_head it is read in full by lm_head)
wbytes = allbytes if tied else allbytes - emb.numel() * emb.element_size()
msgs = [{"role": "user", "content": "Explain in a few paragraphs how a GPU executes a matrix multiplication."}]
try:
    text = tok.apply_chat_template(msgs, tokenize=False, add_generation_prompt=True, enable_thinking=False)
except Exception as e:
    print("chat template failed, using the raw prompt:", e)
    text = msgs[0]["content"]
ids1 = tok(text, return_tensors="pt").input_ids.to(dev)
print(f"gpu={torch.cuda.get_device_name(0)} torch={torch.__version__} transformers={transformers.__version__} "
      f"mps_pct={os.environ.get('CUDA_MPS_ACTIVE_THREAD_PERCENTAGE', 'none')} tag={a.tag}", flush=True)
print(f"model={a.model} class={type(model).__name__} params={nparam} lm_param_bytes={allbytes} step_weight_bytes={wbytes} "
      f"embedding_bytes={emb.numel()*emb.element_size()} lm_head_tied={tied} dtype={emb.dtype} "
      f"prompt_tokens={ids1.shape[1]} new_tokens={a.new_tokens}", flush=True)


def cache_of(out):
    return getattr(out, "cache", None) if getattr(out, "cache", None) is not None else out.past_key_values


def step_kwargs(cache):
    return {"cache": cache} if cache_kw == "cache" else {"past_key_values": cache}


import inspect
cache_kw = "cache" if "cache" in inspect.signature(model.forward).parameters else "past_key_values"


def prefill(b):
    ids = ids1.repeat(b, 1)
    mask = torch.ones_like(ids)
    out = model(input_ids=ids, attention_mask=mask, use_cache=True)
    return out.logits[:, -1].argmax(-1, keepdim=True), mask, cache_of(out)


def step(state, timed=True):
    nxt, mask, cache = state
    mask = torch.cat([mask, torch.ones_like(nxt)], 1)
    if timed:
        sync()
    t = time.perf_counter()
    out = model(input_ids=nxt, attention_mask=mask, use_cache=True, **step_kwargs(cache))
    nxt = out.logits[:, -1].argmax(-1, keepdim=True)
    if timed:
        sync()
    return (nxt, mask, cache_of(out)), time.perf_counter() - t


def sequence(b, n):
    state, ts, toks = prefill(b), [], []
    for _ in range(n):
        toks.append(state[0])
        state, dt = step(state)
        ts.append(dt)
    return ts, torch.cat(toks, 1)


with torch.inference_mode():
    batches = [int(x) for x in a.batches.split(",")]
    for b in batches:  # warm-up
        _, toks = sequence(b, 16)
    print("sample:", repr(tok.decode(sequence(1, 32)[1][0])), flush=True)

    if a.mode == "solo":
        for b in batches:
            ts = []
            for _ in range(a.reps):
                ts += sequence(b, a.new_tokens)[0]
            med = statistics.median(ts)
            g = []
            ids = ids1.repeat(b, 1)
            for _ in range(a.reps):
                sync(); t = time.perf_counter()
                out = model.generate(input_ids=ids, attention_mask=torch.ones_like(ids), do_sample=False,
                                     min_new_tokens=a.new_tokens, max_new_tokens=a.new_tokens)
                sync(); g.append(time.perf_counter() - t)
            assert out.shape[1] - ids.shape[1] == a.new_tokens
            gmed = statistics.median(g)
            print(f"batch={b} step_ms_median={med*1e3:.3f} step_ms_min={min(ts)*1e3:.3f} step_ms_max={max(ts)*1e3:.3f} "
                  f"n={len(ts)} tok_per_s={b/med:.1f} weight_gb={wbytes/1e9:.3f} weight_gbs={wbytes/1e9/med:.1f} "
                  f"generate_s_median={gmed:.3f} generate_tok_per_s={b*a.new_tokens/gmed:.1f} (includes prefill)",
                  flush=True)
        print(f"peak_mem_gb={torch.cuda.max_memory_allocated()/1e9:.1f}")

    elif a.mode == "window":
        from torch.profiler import profile, ProfilerActivity
        from profcat import cuda_ms
        other = {"a": "b", "b": "a"}[a.role]
        f = lambda kind, who, k: os.path.join(a.sync_dir, f"{kind}_{who}_{k}")

        def run(b, cond):
            ts, state, k = [], prefill(b), 0
            while cond():
                if k == a.new_tokens:
                    state, k = prefill(b), 0
                state, dt = step(state)
                ts.append(dt); k += 1
            return ts

        for k, b in enumerate(batches):
            if a.sync_dir:
                open(f("ready", a.role, k), "w").close()
                run(b, lambda: not os.path.exists(os.path.join(a.sync_dir, f"go_{k}")))  # load until the partner is ready
            t0 = time.perf_counter()
            ts = run(b, lambda: time.perf_counter() - t0 < a.duration)
            wall = time.perf_counter() - t0
            med = statistics.median(ts)
            state = prefill(b)
            sync()
            with profile(activities=[ProfilerActivity.CPU, ProfilerActivity.CUDA]) as prof:
                for _ in range(32):
                    state, _ = step(state, timed=False)
                sync()
            allk, mm = cuda_ms(prof)
            print(f"segment={k} batch={b} window_s={wall:.1f} steps={len(ts)} step_ms_median={med*1e3:.3f} "
                  f"tok_per_s_wall={b*len(ts)/wall:.1f} tok_per_s_median={b/med:.1f} "
                  f"kernel_ms_per_step={allk/32:.3f} matmul_kernel_ms_per_step={mm/32:.3f} "
                  f"matmul_kernel_weight_gbs={wbytes/1e9/(mm/32e3):.0f}", flush=True)
            if a.sync_dir:
                open(f("done", a.role, k), "w").close()
                run(b, lambda: not os.path.exists(f("done", other, k)))  # stay loaded while the partner finishes

    else:
        from torch.profiler import profile, ProfilerActivity
        from profcat import report
        for b in batches:
            state = prefill(b)
            sync()
            with profile(activities=[ProfilerActivity.CPU, ProfilerActivity.CUDA]) as prof:
                for _ in range(a.new_tokens):
                    state, _ = step(state, timed=False)
                sync()
            report(prof, f"{a.new_tokens} decode steps, batch {b}", a.new_tokens)
