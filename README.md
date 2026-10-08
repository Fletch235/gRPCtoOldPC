# gRPCtoOldPC

A small language model running on an old desktop, reachable over gRPC.

Hardware this is built for: [SPECS.md](SPECS.md) — i5-7400, 8 GB DDR4, GTX 1050 Ti (4 GB).

```
terminal client ──gRPC──> C++ service ──HTTP──> llama-server ──> GGUF model
                        (single-flight)       (127.0.0.1 only)  (on the GPU)
```

## Output

Instead of letting your old PC collect dust, use it as local AI hardware.
You run one command in a terminal with a prompt, and the model's answer streams
back word by word as it's generated, printed to stdout like any other Unix tool.
When it finishes you get a second line on stderr with the token counts and
timings, so the completion itself pipes cleanly into other commands. Ctrl-C stops
generation immediately and frees the GPU. If you fire off a second prompt while
one is still running, that second command fails instantly with a "busy" error
rather than waiting, there's no queue and no hidden delay.

## Decisions, in implementation order

1. The old PC runs Ubuntu Server, headless, on wired ethernet, with both services managed by systemd and set to restart on failure.
2. Inference runs on the GTX 1050 Ti rather than the CPU, because generation speed is capped by memory bandwidth and the card's ~112 GB/s beats this board's dual-channel DDR4-2400 (~25 GB/s in practice) — measured 14× faster on prefill and 2.1× on generation, see [SPECS.md](SPECS.md).
3. That means installing the NVIDIA driver (`ubuntu-drivers install`) before anything else, since without it llama.cpp silently falls back to CPU.
4. llama.cpp comes from the prebuilt CUDA release asset rather than a source build, which would take 20–40 minutes on this CPU.
5. Specifically the **CUDA 12.x** asset, not 13.x — CUDA 13 dropped support for Pascal, and the 1050 Ti is compute capability 6.1.
6. The tarball does not bundle the CUDA runtime, so `libcudart12`, `libcublas12` and `libgomp1` come from `apt` — without them `libggml-cuda.so` fails to load and llama.cpp falls back to the CPU silently.
7. Inference is handled by `llama-server`, llama.cpp's built-in HTTP server, rather than linking against llama.cpp's C API directly — it already does model loading, KV cache, and slot scheduling.
8. `llama-server` binds to `127.0.0.1` only, so nothing but the local gRPC service can reach it.
9. It runs with `-ngl 99` to offload every layer to the GPU; a partial offload would make generation fall back to RAM bandwidth for the layers left behind, which is the thing we're avoiding.
10. The model is a 3–4B parameter GGUF at Q4_K_M, which lands at 2.2–2.5 GB and leaves room inside 4 GB of VRAM for the KV cache and compute buffers.
11. Context starts at `-c 4096` because it is VRAM-limited here, not RAM-limited, and stays there — measured use is 3091 of 4031 MiB, leaving no room for 8192.
12. It runs with `--parallel 1`, giving one slot with one KV cache that holds the full context and reuses the conversation prefix across turns.
13. `-t 4` matches the four physical cores; the i5-7400 has no hyperthreading, so threads and cores are the same number.
14. The gRPC contract is a single service, `llm.v1.LLM`, with one method, `Generate`, defined as a server streaming RPC so tokens arrive as they're produced.
15. Requests carry `repeated Message messages` rather than a single prompt string, so the client owns conversation history and the server stays stateless — a single-shot call is just a one-element list.
16. Responses are a `oneof` of `Delta` (a chunk of text) and `Done` (sent exactly once at the end), which puts the stream's shape in the schema instead of in a comment.
17. Cancellation is not modelled in the payload — Ctrl-C closes the stream, the RPC ends with gRPC status `CANCELLED`, and no `Done` is sent.
18. That context cancellation is propagated into the HTTP request to `llama-server`, otherwise an abandoned request keeps the GPU busy until it finishes on its own.
19. The `Done` message reports prompt and completion token counts plus prefill and generation times, which is how you'll measure what the hardware actually delivers.
20. Both programs are C++ against gRPC's reference implementation, with `libgrpc++-dev`, `protobuf-compiler-grpc`, `libcurl4-openssl-dev` and `nlohmann-json3-dev` all from `apt` — libcurl and nlohmann/json stand in for the HTTP client and JSON parser the standard library doesn't have, and nothing gets built from source.
21. The cost of that choice is gRPC-Web: the C++ stack doesn't speak it, so the deferred website will need a proxy such as Envoy in front of this service. Accepted, because the proxy is only needed the day the website exists.
22. Single-flight is enforced in the service with a `std::mutex` and `try_lock`, which refuses rather than waits, because `llama-server` would otherwise queue extra requests invisibly and without bound.
23. A rejected request returns `RESOURCE_EXHAUSTED` rather than `UNAVAILABLE`, since `UNAVAILABLE` is retried automatically by default and would cause retry storms against an already-saturated machine.
24. Server reflection is enabled so `grpcurl` works without a copy of the `.proto` file; it needs `-lgrpc++_reflection` named explicitly, since `grpc++.pc` leaves it out.
25. Day-to-day use goes through a small purpose-built client that prints raw text to stdout and stats to stderr, since `grpcurl` emits one JSON object per token chunk.
26. `llmd` binds `127.0.0.1` too, so for now the client reaches it over an SSH tunnel. TLS, authentication, and real network exposure are deliberately deferred and will be addressed together with the website.

## Layout

| | |
|---|---|
| [llm/v1/llm.proto](llm/v1/llm.proto) | the contract — decisions 14–19 |
| [generate.sh](generate.sh) | `protoc` invocation; output lands in `gen/`, which is not committed |
| [Makefile](Makefile) | `pkg-config` against the apt packages, two binaries into `bin/` |
| [src/llmd.cpp](src/llmd.cpp) | the service: single-flight, SSE → stream, cancellation |
| [src/llm.cpp](src/llm.cpp) | the terminal client: text to stdout, stats to stderr |
| [deploy/](deploy/) | the two systemd units |

## Deploy

Everything below runs over SSH on the old PC. Phases 1–3 (OS, driver,
llama.cpp, model, benchmark) are already done — see [SPECS.md](SPECS.md).

**Toolchain.** All from `apt`, nothing built from source:

```bash
sudo apt install -y build-essential pkg-config git \
  libgrpc++-dev protobuf-compiler protobuf-compiler-grpc \
  libcurl4-openssl-dev nlohmann-json3-dev
```

**Build.** `make` runs `generate.sh` for you when the `.proto` is newer than
the generated sources:

```bash
git clone https://github.com/Fletch235/gRPCtoOldPC.git ~/gRPCtoOldPC
cd ~/gRPCtoOldPC
make
```

**Install.** An unprivileged system user with no login shell and no home:

```bash
sudo useradd --system --no-create-home --shell /usr/sbin/nologin llm || true
sudo install -d -o root -g root /opt/llm/bin
sudo install -m 755 bin/llmd bin/llm /opt/llm/bin/
sudo ln -sfn /opt/llm/bin/llm /usr/local/bin/llm
sudo chown -R llm:llm /opt/llama.cpp/models
```

**Run both services.** Stop any `llama-server` still running by hand first,
otherwise port 8080 is taken:

```bash
pkill -f llama-server || true
sudo cp deploy/*.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now llama-server llmd
```

**Check.** The model takes a few seconds to load, so wait on `/health` before
prompting:

```bash
until curl -sf http://127.0.0.1:8080/health >/dev/null; do sleep 2; done
systemctl is-active llama-server llmd
llm 'name three uses for an old desktop PC'
```

**Check single-flight.** The second one should fail immediately with
`resource_exhausted`, not wait:

```bash
llm -n 400 'write a long story about a server rack' >/dev/null &
sleep 1 && llm 'hello'
wait
```

**Check it survives a reboot.** This is the whole point of the systemd units:

```bash
sudo reboot
# then, once it is back
systemctl is-active llama-server llmd && llm 'still here?'
```

**From your own machine**, tunnel to the box — `llmd` listens on loopback
only, so this is the only way in until TLS and auth land with the website
(decision 26):

```bash
ssh -N -L 50051:127.0.0.1:50051 <user>@<box-ip> &
LLM_ADDR=localhost:50051 llm 'hello from my laptop'
```
