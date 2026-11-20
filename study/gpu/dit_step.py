#!/usr/bin/env python
"""Time one Z-Image-Turbo DiT forward (1024x1024, batch 1, BF16).

The pipeline is run once as the model card does (9 scheduler steps = 8 DiT
forwards); the eight transformer calls are captured and then replayed back to
back, so the timed region is the transformer alone (no text encoder, no VAE).

modes
  solo     median ms per forward over --reps passes of the 8 captured forwards,
           plus the end-to-end time of whole images
  window   run forwards continuously for --duration seconds, then profile three
           forwards for the time the GPU spent in kernels. With --sync-dir two
           processes measure the same windows, --segments of them
  profile  torch.profiler CUDA time by operator for the 8 forwards of one image
"""
import argparse, os, statistics, time
import torch

p = argparse.ArgumentParser()
p.add_argument("--mode", default="solo", choices=["solo", "window", "profile"])
p.add_argument("--model", default="Tongyi-MAI/Z-Image-Turbo")
p.add_argument("--size", type=int, default=1024)
p.add_argument("--reps", type=int, default=5)
p.add_argument("--duration", type=float, default=30)
p.add_argument("--sync-dir", help="co-run: directory of ready/go/done files shared with the partner")
p.add_argument("--role", default="a", choices=["a", "b"])
p.add_argument("--segments", type=int, default=1)
p.add_argument("--tag", default="")
p.add_argument("--save-image")
a = p.parse_args()

from diffusers import ZImagePipeline
import diffusers

dev = "cuda"
sync = torch.cuda.synchronize
pipe = ZImagePipeline.from_pretrained(a.model, torch_dtype=torch.bfloat16, low_cpu_mem_usage=a.mode != "solo")
pipe.to(dev)
tr = pipe.transformer
wbytes = sum(q.numel() * q.element_size() for q in tr.parameters())
nparam = sum(q.numel() for q in tr.parameters())
prompt = "A lighthouse on a rocky coast at dusk, warm window light, crashing waves, photorealistic"


def image(output_type="pil"):
    return pipe(prompt=prompt, height=a.size, width=a.size, num_inference_steps=9, guidance_scale=0.0,
                generator=torch.Generator(dev).manual_seed(42), output_type=output_type).images[0]


calls = []
hook = tr.register_forward_pre_hook(lambda m, args, kwargs: calls.append((args, kwargs)), with_kwargs=True)
with torch.inference_mode():
    img = image()
hook.remove()
if a.save_image:
    img.save(a.save_image)
pipe.set_progress_bar_config(disable=True)
shapes = [[tuple(y.shape) for y in x] if isinstance(x, (list, tuple)) else tuple(getattr(x, "shape", ())) for x in calls[0][0]]
print(f"gpu={torch.cuda.get_device_name(0)} torch={torch.__version__} diffusers={diffusers.__version__} "
      f"mps_pct={os.environ.get('CUDA_MPS_ACTIVE_THREAD_PERCENTAGE', 'none')} tag={a.tag}", flush=True)
print(f"model={a.model} size={a.size} forwards_per_image={len(calls)} forward_input_shapes={shapes} "
      f"transformer_params={nparam} "
      f"transformer_weight_bytes={wbytes} dtype={next(tr.parameters()).dtype}", flush=True)


def fwd(i):
    args, kwargs = calls[i % len(calls)]
    sync()
    t = time.perf_counter()
    tr(*args, **kwargs)
    sync()
    return time.perf_counter() - t


with torch.inference_mode():
    for i in range(len(calls)):  # warm-up
        fwd(i)

    if a.mode == "solo":
        ts = [fwd(i) for i in range(a.reps * len(calls))]
        med = statistics.median(ts)
        print(f"fwd_ms_median={med*1e3:.2f} fwd_ms_min={min(ts)*1e3:.2f} fwd_ms_max={max(ts)*1e3:.2f} "
              f"n={len(ts)} weight_gb={wbytes/1e9:.3f} weight_gbs={wbytes/1e9/med:.1f}")
        e2e, lat = [], []
        for _ in range(a.reps):
            for kind, acc in (("pil", e2e), ("latent", lat)):
                sync(); t = time.perf_counter(); image(kind); sync(); acc.append(time.perf_counter() - t)
        print(f"image_s_median={statistics.median(e2e):.3f} (text encoder + {len(calls)} forwards + VAE decode) "
              f"image_latent_s_median={statistics.median(lat):.3f} (no VAE decode) n={a.reps}")
        print(f"peak_mem_gb={torch.cuda.max_memory_allocated()/1e9:.1f}")

    elif a.mode == "window":
        from torch.profiler import profile, ProfilerActivity
        from profcat import cuda_ms, report
        other = {"a": "b", "b": "a"}[a.role]
        f = lambda kind, who, k: os.path.join(a.sync_dir, f"{kind}_{who}_{k}")
        i = 0
        for k in range(a.segments):
            if a.sync_dir:
                open(f("ready", a.role, k), "w").close()
                while not os.path.exists(os.path.join(a.sync_dir, f"go_{k}")):
                    fwd(i); i += 1  # keep the GPU loaded until the partner is ready
            ts, t0 = [], time.perf_counter()
            while time.perf_counter() - t0 < a.duration:
                ts.append(fwd(i)); i += 1
            wall = time.perf_counter() - t0
            med = statistics.median(ts)
            with profile(activities=[ProfilerActivity.CPU, ProfilerActivity.CUDA]) as prof:
                for j in range(3):
                    tr(*calls[j][0], **calls[j][1])
                sync()
            allk, mm = cuda_ms(prof)
            print(f"segment={k} window_s={wall:.1f} forwards={len(ts)} fwd_ms_median={med*1e3:.2f} "
                  f"fwd_per_s_wall={len(ts)/wall:.3f} fwd_per_s_median={1/med:.3f} "
                  f"kernel_ms_per_fwd={allk/3:.2f} matmul_kernel_ms_per_fwd={mm/3:.2f}", flush=True)
            if a.sync_dir:
                open(f("done", a.role, k), "w").close()
                while not os.path.exists(f("done", other, k)):
                    fwd(i); i += 1  # stay loaded while the partner finishes

    else:
        from torch.profiler import profile, ProfilerActivity
        from profcat import report
        with profile(activities=[ProfilerActivity.CPU, ProfilerActivity.CUDA]) as prof:
            for i in range(len(calls)):
                tr(*calls[i][0], **calls[i][1])
            sync()
        report(prof, f"{len(calls)} DiT forwards", len(calls))
