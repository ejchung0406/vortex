# Co-run scenario: local image generation beside an LLM chat

Chosen with the user on 2026-11-20. Values are read from the models'
Hugging Face config files unless marked as derived or assumed.

## Bandwidth-bound task: Qwen3.5-9B decode, a few sessions

https://huggingface.co/Qwen/Qwen3.5-9B (`config.json`, `text_config`)

| | |
|---|---|
| hidden_size | 4096 |
| intermediate_size (FFN) | 12288 |
| num_hidden_layers | 32 (24 linear-attention "Gated DeltaNet" + 8 full attention, pattern 3:1) |
| heads | 16 query, 4 KV, head_dim 256 |
| dtype | bfloat16 |
| parameters | 9B (card); 10B with the vision encoder |
| MoE | no expert fields in the config; one dense FFN per layer |

Derived: an FFN matrix is 4096 x 12288 = 50.3 M weights = 101 MB in BF16; a
decode step reads every weight once, about 18 GB. With 4 sessions the GEMM is
M = 4, K = 4096 or 12288. Memory-bound: at 273 GB/s (an LPDDR5X laptop part)
the weights alone allow about 15 tokens/s, at 4.8 TB/s (H200) about 270.

## Compute-bound task: Z-Image-Turbo, one denoising step

https://huggingface.co/Tongyi-MAI/Z-Image-Turbo (`transformer/config.json`)

| | |
|---|---|
| class | ZImageTransformer2DModel, single-stream DiT |
| dim | 3840 |
| n_layers | 30 (+2 refiner layers) |
| n_heads | 30 (128 per head, derived) |
| patch size | 2, in_channels 16 |
| steps | 8 DiT forwards, no CFG |
| parameters | 6B, Apache 2.0, "fits 16G VRAM" |

Assumed, not on the card: an 8x VAE, so 1024 x 1024 is a 128 x 128 latent and
64 x 64 = 4096 image tokens after the 2 x 2 patch, plus a few hundred text
tokens. Then every linear layer is a GEMM with M about 4096 and K = 3840: the
same weights as a decode-sized matrix but about a thousand times the
multiplies per weight byte. Compute-bound.

Alternative considered: FLUX.2 [klein] 4B (inner dim 3072, 5 double + 20
single layers, 4 steps, about 13 GB VRAM).

## In the simulator (`gemm_block`, roles split by SM)

| Role | Real | Simulated |
|---|---|---|
| memory-bound | M = 4, K = 4096 | M = 16 (one tensor-core tile), K = 4096 |
| compute-bound | M = 4096, K = 3840 | M = 64..256 (to be chosen from the 1-SM sweep `msw_*`), K = 4096 |

Both are scaled-down but stay in their regime; say so where the numbers are
used. Open: the compute role currently streams fresh weights like the decode
role, whereas a DiT layer's 30 MB matrix is reused across 4096 tokens — make
the compute role re-read a small weight region. Open: measure a real Z-Image
step's memory bandwidth on the H200.

## Measured on a real H200 (2026-11-20, scripts `gpu/dit_step.py`, `gpu/llm_decode.py`, `gpu/corun_models.sh`; raw `results_gpu/h200_{zimage,qwen,models}_*`)

Software: torch 2.15, transformers and diffusers from git main; Z-Image-Turbo BF16 1024x1024; Qwen3.5-9B text-only class, BF16, KV cache, 128 new tokens.

| | Z-Image DiT forward | Qwen decode step |
|---|---|---|
| time (solo, 100% of SMs) | 153.6 ms, GPU-bound (kernel time 148 ms) | 26.8-27.6 ms wall at batch 1-64, of which GPU kernels 7.2-9.3 ms |
| weights touched | 12.31 GB | 15.87 GB (17.91 GB minus the 2.03 GB input embedding) |
| weight traffic | 80 GB/s = 1.7% of 4.8 TB/s | over matmul kernel time: 3.1-3.2 TB/s = 65-68% |

Decode wall-clock in HF transformers is host-bound (about 20 of 27 ms per step is not GPU time); only the kernel-time view shows the GPU.

Share of SMs (MPS active-thread percentage), each alone:

| % of SMs | 100 | 80 | 70 | 60 | 40 | 20 | 10 | 5 |
|---|---|---|---|---|---|---|---|---|
| DiT throughput, relative | 1.00 | 1.01 | 0.90 | 0.81 | 0.57 | 0.30 | 0.14 | 0.095 |
| decode matmul weight traffic, GB/s (batch 1) | 3184 | 3184 | 3216 | 3100 | 2842 | 2300 | 1233 | 831 |
| decode matmul throughput, relative | 1.00 | 1.00 | 1.01 | 0.97 | 0.89 | 0.72 | 0.39 | 0.26 |

So the image step is compute-bound (proportional to SMs below ~70%) and the decode kernels are bandwidth-bound (72% of full speed on a fifth of the SMs), and with few SMs decode falls to roughly 95-125 GB/s per SM (13 SMs: 1233; 6-7 SMs: 831), the per-SM request limit.

Both at once (MPS, complementary shares; ratio to solo at the same share): DiT 0.93-0.96; decode wall-clock 0.97-0.98, decode GPU kernel time 0.71-0.82 (matmul traffic at an 80/20 split: 2300 -> 1741 GB/s). MPS shares do not pin the two clients to disjoint SMs, so this includes any SM sharing; single 30 s windows per point.

Operator mix (CUDA time): DiT linear/matmul 45%, elementwise 26%, attention 12%; decode linear/matmul 55-68%.

Not done: a low-overhead decode runtime (the host bound hides the GPU effect in tokens/s); repeated runs for the MPS points.
