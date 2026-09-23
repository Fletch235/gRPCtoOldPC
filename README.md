# gRPCtoOldPC

A small language model running on an old desktop, reachable over gRPC.


```
terminal client ──gRPC──> Go service ──HTTP──> llama-server ──> GGUF model
                        (single-flight)      (127.0.0.1 only)
```

## Output

Instead of letting your old PC collect dust, use it as local AI harware.
You run one command in a terminal with a prompt, and the model's answer streams
back word by word as it's generated, printed to stdout like any other Unix tool.
When it finishes you get a second line on stderr with the token counts and
timings, so the completion itself pipes cleanly into other commands. Ctrl-C stops
generation immediately and frees the CPU. If you fire off a second prompt while
one is still running, that second command fails instantly with a "busy" error
rather than waiting, there's no queue and no hidden delay.

## Decisions, in implementation order

1. The old PC runs Ubuntu Server, headless, with both services managed by systemd and set to restart on failure.
2. Inference is handled by `llama-server`, llama.cpp's built-in HTTP server, rather than linking against llama.cpp's C API directly — it already does model loading, KV cache, and slot scheduling.
3. `llama-server` binds to `127.0.0.1` only, so nothing but the local gRPC service can reach it.
4. It runs with `--parallel 1`, giving one slot with one KV cache that holds the full context and reuses the conversation prefix across turns.
5. The model is a 3–4B parameter GGUF at Q4_K_M quantization, sized to fit entirely in RAM alongside the KV cache, because CPU generation speed is capped by memory bandwidth divided by model file size.
6. The gRPC contract is a single service, `llm.v1.LLM`, with one method, `Generate`, defined as a server streaming RPC so tokens arrive as they're produced.
7. Requests carry `repeated Message messages` rather than a single prompt string, so the client owns conversation history and the server stays stateless — a single-shot call is just a one-element list.
8. Responses are a `oneof` of `Delta` (a chunk of text) and `Done` (sent exactly once at the end), which puts the stream's shape in the schema instead of in a comment.
9. Cancellation is not modelled in the payload — Ctrl-C closes the stream, the RPC ends with gRPC status `CANCELLED`, and no `Done` is sent.
10. That context cancellation is propagated into the HTTP request to `llama-server`, otherwise an abandoned request keeps the CPU pegged until it finishes on its own.
11. The `Done` message reports prompt and completion token counts plus prefill and generation times, which is how you'll measure what the hardware actually delivers.
12. The server is written in Go using `connect-go`, which serves native gRPC today and gRPC-Web from the same handler later, so the deferred website won't need a proxy.
13. Single-flight is enforced in the gRPC service with a capacity-1 channel and a non-blocking `select`, because `llama-server` would otherwise queue extra requests invisibly and without bound.
14. A rejected request returns `RESOURCE_EXHAUSTED` rather than `UNAVAILABLE`, since `UNAVAILABLE` is retried automatically by default and would cause retry storms against an already-saturated machine.
15. Server reflection is enabled so `grpcurl` works without a copy of the `.proto` file.
16. Day-to-day use goes through a small purpose-built Go client that prints raw text to stdout and stats to stderr, since `grpcurl` emits one JSON object per token chunk.
17. TLS, authentication, and network exposure are deliberately deferred and will be addressed together with the website.
