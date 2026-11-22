#!/usr/bin/env python
"""Experiment C: standalone check of the BF16 GEMMs of Qwen3.5-9B's Linear
layers (y = x W^T, x [M, in], W [out, in]) against an FP32 reference, for the
BLAS back ends torch offers, in this process's SM allotment (run under MPS
percentages). Prints NaN counts and max relative error per shape."""
import os, sys, torch, ctypes
lib = ctypes.CDLL("/tmp/echung67/gng/libgng_smcount.so"); attr = ctypes.c_int(0); mx = ctypes.c_int(0)
torch.zeros(1, device="cuda"); nsm = lib.gng_count_sms(ctypes.byref(attr), ctypes.byref(mx))
shapes = [(4096, 12288), (12288, 4096), (4096, 4096), (4096, 1024), (4096, 8192), (4096, 248320 // 8)]
torch.manual_seed(0)
pct = os.environ.get("CUDA_MPS_ACTIVE_THREAD_PERCENTAGE", "none")
for backend in ("cublaslt", "cublas"):
    try:
        torch.backends.cuda.preferred_blas_library(backend)
    except Exception as e:
        print("backend", backend, "unavailable", e); continue
    for (fin, fout) in shapes:
        W = (torch.randn(fout, fin, device="cuda") * 0.02).to(torch.bfloat16)
        for M in (1, 4, 16, 26, 64):
            x = torch.randn(M, fin, device="cuda").to(torch.bfloat16)
            y = torch.nn.functional.linear(x, W)
            ref = x.float() @ W.float().t()
            nan = int((~torch.isfinite(y)).sum())
            err = float(((y.float() - ref).abs().max() / ref.abs().max()))
            print(f"mps_pct={pct} smid_distinct={nsm} backend={backend} in={fin} out={fout} M={M} nonfinite={nan} of {y.numel()} max_rel_err={err:.2e} {'BAD' if nan or err > 0.05 else 'ok'}", flush=True)
