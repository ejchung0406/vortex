#!/usr/bin/env python
"""Experiment C diagnosis: why Qwen3.5-9B decode printed '!!!!' under MPS.

One greedy prefill + 32 decode steps (batch 1, the llm_decode.py prompt), then
prints: the MPS percentage, the SM count the process is given (torch device
property, Triton's driver property, and the number of distinct %smid values a
probe kernel reaches, from libgng_smcount.so), whether the prefill logits are
finite, the sample text, and with --hooks the first module (in call order)
whose inputs are finite but whose output is not.

--fake-sms N   make torch/Triton/FLA report N SMs (no MPS needed): tests whether
               a kernel's grid choice from the SM count produces the bad output
--poison       fill the CUDA caching allocator with NaN before loading, so any
               read of uninitialised memory turns into NaN
"""
import argparse, ctypes, os, sys, time
import torch

p = argparse.ArgumentParser()
p.add_argument("--model", default="/tmp/echung67/models/qwen")
p.add_argument("--fake-sms", type=int, default=0)
p.add_argument("--poison", action="store_true")
p.add_argument("--hooks", action="store_true")
p.add_argument("--steps", type=int, default=32)
p.add_argument("--tag", default="")
a = p.parse_args()

lib = ctypes.CDLL(os.environ.get("GNG_SMCOUNT", "/tmp/echung67/gng/libgng_smcount.so"))
attr = ctypes.c_int(0); mx = ctypes.c_int(0)
torch.cuda.init()
torch.zeros(1, device="cuda")
n_smid = lib.gng_count_sms(ctypes.byref(attr), ctypes.byref(mx))
torch_sms = torch.cuda.get_device_properties(0).multi_processor_count
import triton
tri_sms = triton.runtime.driver.active.utils.get_device_properties(0)["multiprocessor_count"]

if a.fake_sms:
    import triton.runtime.driver as _d
    real = triton.runtime.driver.active.utils.get_device_properties
    def fake(i, _real=real):
        d = dict(_real(i)); d["multiprocessor_count"] = a.fake_sms; return d
    triton.runtime.driver.active.utils.get_device_properties = fake
    import fla.utils, fla.utils.hardware as fh
    fh.get_multiprocessor_count = lambda *x, **k: a.fake_sms
    fla.utils.get_multiprocessor_count = lambda *x, **k: a.fake_sms
    for mname, m in list(sys.modules.items()):
        if mname.startswith("fla") and hasattr(m, "get_multiprocessor_count"):
            m.get_multiprocessor_count = lambda *x, **k: a.fake_sms

if a.poison:
    free, total = torch.cuda.mem_get_info()
    blocks = []
    try:
        while True:
            t = torch.empty(1 << 30, dtype=torch.uint8, device="cuda")
            t.fill_(0xFF)  # 0xFFFF in BF16 / 0xFFFFFFFF in FP32 = NaN
            blocks.append(t)
            if len(blocks) * (1 << 30) > free - (40 << 30):
                break
    except RuntimeError:
        pass
    del blocks  # memory stays in PyTorch's cache, NaN-filled

from transformers import AutoModelForCausalLM, AutoTokenizer
tok = AutoTokenizer.from_pretrained(a.model)
model = AutoModelForCausalLM.from_pretrained(a.model, dtype=torch.bfloat16).to("cuda").eval()
msgs = [{"role": "user", "content": "Explain in a few paragraphs how a GPU executes a matrix multiplication."}]
text = tok.apply_chat_template(msgs, tokenize=False, add_generation_prompt=True, enable_thinking=False)
ids = tok(text, return_tensors="pt").input_ids.to("cuda")

first_bad = []
if a.hooks:
    def fin(x):
        if torch.is_tensor(x):
            return bool(torch.isfinite(x).all()) if x.is_floating_point() else True
        if isinstance(x, (tuple, list)):
            return all(fin(y) for y in x)
        if isinstance(x, dict):
            return all(fin(y) for y in x.values())
        if hasattr(x, "to_tuple"):
            try:
                return all(fin(y) for y in x.to_tuple())
            except Exception:
                return True
        return True
    def hook(name):
        def h(mod, inp, kw, out):
            if first_bad:
                return
            if fin(inp) and fin(kw) and not fin(out):
                first_bad.append(f"{name} ({type(mod).__name__})")
        return h
    for name, mod in model.named_modules():
        mod.register_forward_hook(hook(name), with_kwargs=True)

def cache_of(out):
    return getattr(out, "cache", None) if getattr(out, "cache", None) is not None else out.past_key_values
import inspect
kw = "cache" if "cache" in inspect.signature(model.forward).parameters else "past_key_values"
with torch.inference_mode():
    mask = torch.ones_like(ids)
    out = model(input_ids=ids, attention_mask=mask, use_cache=True)
    pre_finite = bool(torch.isfinite(out.logits).all())
    pre_bad = first_bad[0] if first_bad else "-"
    nxt = out.logits[:, -1].argmax(-1, keepdim=True); cache = cache_of(out)
    toks = [nxt]
    nonfinite_steps = 0
    for _ in range(a.steps - 1):
        mask = torch.cat([mask, torch.ones_like(nxt)], 1)
        out = model(input_ids=nxt, attention_mask=mask, use_cache=True, **{kw: cache})
        nonfinite_steps += int(not bool(torch.isfinite(out.logits).all()))
        nxt = out.logits[:, -1].argmax(-1, keepdim=True); cache = cache_of(out); toks.append(nxt)
    torch.cuda.synchronize()
sample = tok.decode(torch.cat(toks, 1)[0])
print(f"tag={a.tag} mps_pct={os.environ.get('CUDA_MPS_ACTIVE_THREAD_PERCENTAGE', 'none')} fake_sms={a.fake_sms} poison={int(a.poison)} "
      f"torch_sms={torch_sms} triton_sms={tri_sms} attr_sms={attr.value} smid_distinct={n_smid} smid_max={mx.value} "
      f"prefill_logits_finite={int(pre_finite)} decode_steps_nonfinite={nonfinite_steps}/{a.steps - 1} "
      f"first_nonfinite_module={pre_bad if a.hooks else 'n/a'} later_first_bad={first_bad[0] if first_bad else '-'} "
      f"not_bang={int(not sample.startswith('!!!!'))} same_as_solo_ref={int(sample.startswith('A GPU executes matrix multiplication by leveraging'))} sample={sample[:80]!r}", flush=True)
