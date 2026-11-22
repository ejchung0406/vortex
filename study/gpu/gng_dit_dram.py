#!/usr/bin/env python
"""Experiment C extra: full DRAM traffic of one Z-Image-Turbo DiT forward (1024x1024, BF16).
Run under: ncu --profile-from-start off --metrics dram__bytes_read.sum,dram__bytes_write.sum,gpu__time_duration.sum --csv
The pipeline runs once to capture the transformer inputs; one warm forward; then one
forward between cudaProfilerStart/Stop. Prints the transformer's weight bytes."""
import torch
from diffusers import ZImagePipeline
pipe = ZImagePipeline.from_pretrained("/tmp/echung67/models/zimage", torch_dtype=torch.bfloat16).to("cuda")
tr = pipe.transformer
calls = []
h = tr.register_forward_pre_hook(lambda m, args, kwargs: calls.append((args, kwargs)), with_kwargs=True)
with torch.inference_mode():
    pipe(prompt="A lighthouse on a rocky coast at dusk", height=1024, width=1024, num_inference_steps=9, guidance_scale=0.0,
         generator=torch.Generator("cuda").manual_seed(42), output_type="latent")
h.remove()
args, kwargs = calls[0]
with torch.inference_mode():
    tr(*args, **kwargs); torch.cuda.synchronize()
    torch.cuda.cudart().cudaProfilerStart()
    tr(*args, **kwargs); torch.cuda.synchronize()
    torch.cuda.cudart().cudaProfilerStop()
print("transformer_weight_bytes=%d" % sum(q.numel() * q.element_size() for q in tr.parameters()), flush=True)
