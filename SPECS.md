# Server hardware

Collected 2026-10-07. Benchmarks 2026-10-08.

| | |
|---|---|
| OS | Ubuntu Server, headless, wired ethernet |
| CPU | Intel i5-7400 — 4 cores / 4 threads (no HT), Kaby Lake, 3.0 GHz base / 3.5 turbo |
| SIMD | AVX2 + FMA, no AVX-512 |
| RAM | 8 GB DDR4-2400, 2 × 4 GB dual-channel — both slots full, board max 32 GB |
| | 6.4 GB available, 4 GB swap |
| GPU | NVIDIA GTX 1050 Ti — GP107, Pascal (compute 6.1), 4031 MiB usable, ~112 GB/s |
| Disk | 477 GB `sda`; 86 GB free on `/`; ~375 GB unallocated in the LVM volume group |

## Measured

llama.cpp `b11480`, CUDA 12.8 build, Qwen3-4B-Instruct-2507 Q4_K_M (2.32 GiB),
`-ngl 99 -c 4096 --parallel 1 -t 4`.

| | GPU (`-ngl 99`) | CPU only |
|---|---|---|
| Prefill (`pp512`) | 359 t/s | 25 t/s |
| Generation (`tg128`) | 22.3 t/s | 10.6 t/s |

VRAM at `-c 4096`: **3091 of 4031 MiB used, ~940 MiB free.**

Generation lands at 49% of the theoretical 45 t/s (112 GB/s ÷ 2.49 GB). That's
expected, not a misconfiguration — GP107 has 768 cores across 6 SMs, too little
parallelism to saturate its own memory bus on a memory-bound matvec. The ~90%
efficiency figure applies to much larger cards.

## What this constrains

**Run inference on the GPU.** Prefill is 14× faster and generation 2.1×. The
prefill gap is what matters most in practice, since it's what you wait on before
the first token appears.

**Weights budget ≈ 2.4 GB.** 4031 MiB less KV cache and compute buffers.

**Stay at `-c 4096`.** Only ~940 MiB free, and 8192 would want another ~600 MiB
of KV cache plus headroom.

**`-t 4`.** Four real cores, no hyperthreading.

**If 22 t/s ever feels slow**, the lever is a smaller model, not tuning —
Qwen2.5-3B Q4_K_M is 1.8 GB and would run ~28 t/s.

**8 GB RAM is the hard ceiling for anything CPU-side.** No free DIMM slots.

## Setup gotchas found

- The llama.cpp CUDA tarball does **not** bundle the CUDA runtime:
  `sudo apt install libcudart12 libcublas12 libgomp1`. Without them
  `libggml-cuda.so` fails to load and llama.cpp falls back to CPU **silently** —
  the tell is ~10 t/s generation and ~25 t/s prefill.
- Use the CUDA **12.x** asset. CUDA 13 dropped Pascal support.
- llama.cpp's GitHub `/releases/latest` returns a bogus `v0.5.0` tag with no
  binaries; resolve the real `b<N>` tag from `/releases`.

## Not yet captured

- Disk type, SSD vs spinning: `lsblk -d -o NAME,ROTA` (`1` = spinning)
