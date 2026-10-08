// llm sends one prompt to llmd and streams the answer to stdout.
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>

#include <grpcpp/grpcpp.h>

#include "llm/v1/llm.grpc.pb.h"

using namespace llm::v1;

namespace {

volatile std::sig_atomic_t g_interrupted = 0;

void on_sigint(int) {
  std::signal(SIGINT, SIG_DFL);  // a second Ctrl-C kills outright
  g_interrupted = 1;
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
  return s;
}

std::string env_or(const char* key, const char* fallback) {
  const char* v = std::getenv(key);
  return (v != nullptr && v[0] != '\0') ? v : fallback;
}

std::string read_stdin() {
  std::string out;
  char buf[4096];
  for (size_t n; (n = std::fread(buf, 1, sizeof buf, stdin)) > 0;) out.append(buf, n);
  return out;
}

std::string stats(const Done& d) {
  std::string reason = FinishReason_Name(d.finish_reason());
  constexpr std::string_view kPrefix = "FINISH_REASON_";
  if (reason.compare(0, kPrefix.size(), kPrefix) == 0) reason.erase(0, kPrefix.size());
  for (char& c : reason) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

  std::string out = reason + ": " + std::to_string(d.prompt_tokens()) + " prompt + " +
                    std::to_string(d.completion_tokens()) + " completion tokens, prefill " +
                    std::to_string(d.prefill_ms()) + "ms, generate " +
                    std::to_string(d.generate_ms()) + "ms";
  if (d.generate_ms() > 0) {
    char rate[32];
    std::snprintf(rate, sizeof rate, " (%.1f tok/s)",
                  d.completion_tokens() * 1000.0 / static_cast<double>(d.generate_ms()));
    out += rate;
  }
  return out;
}

// The C++ API has no public status-code-to-name function.
const char* code_name(grpc::StatusCode c) {
  switch (c) {
    case grpc::StatusCode::CANCELLED: return "cancelled";
    case grpc::StatusCode::INVALID_ARGUMENT: return "invalid_argument";
    case grpc::StatusCode::DEADLINE_EXCEEDED: return "deadline_exceeded";
    case grpc::StatusCode::UNIMPLEMENTED: return "unimplemented";
    case grpc::StatusCode::INTERNAL: return "internal";
    case grpc::StatusCode::RESOURCE_EXHAUSTED: return "resource_exhausted";
    case grpc::StatusCode::UNAVAILABLE: return "unavailable";
    default: return "error";
  }
}

void usage() {
  std::fprintf(stderr,
               "usage: llm [flags] prompt...   (or pipe the prompt on stdin)\n"
               "  -s HOST:PORT   llmd address (default $LLM_ADDR or localhost:50051)\n"
               "  -system TEXT   system prompt\n"
               "  -n N           max tokens (0 leaves it to the server)\n"
               "  -temp F        temperature (negative leaves it to the server)\n"
               "  -q             omit the stats line on stderr\n");
}

}  // namespace

int main(int argc, char** argv) {
  std::string target = env_or("LLM_ADDR", "localhost:50051");
  std::string system_prompt;
  std::string prompt;
  int max_tokens = 0;
  double temperature = -1;
  bool quiet = false;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const bool has_value = i + 1 < argc;
    if (arg == "-s" && has_value) {
      target = argv[++i];
    } else if (arg == "-system" && has_value) {
      system_prompt = argv[++i];
    } else if (arg == "-n" && has_value) {
      max_tokens = std::atoi(argv[++i]);
    } else if (arg == "-temp" && has_value) {
      temperature = std::atof(argv[++i]);
    } else if (arg == "-q") {
      quiet = true;
    } else if (arg == "-h" || arg == "--help") {
      usage();
      return 0;
    } else if (!arg.empty() && arg.front() == '-' && arg != "-") {
      std::fprintf(stderr, "llm: unknown flag %s\n", argv[i]);
      usage();
      return 2;
    } else {
      if (!prompt.empty()) prompt += ' ';
      prompt += argv[i];
    }
  }

  prompt = std::string(trim(prompt));
  if (prompt.empty()) prompt = std::string(trim(read_stdin()));
  if (prompt.empty()) {
    usage();
    return 2;
  }

  GenerateRequest req;
  if (!system_prompt.empty()) {
    Message* m = req.add_messages();
    m->set_role(ROLE_SYSTEM);
    m->set_content(system_prompt);
  }
  Message* m = req.add_messages();
  m->set_role(ROLE_USER);
  m->set_content(prompt);
  if (max_tokens > 0) req.set_max_tokens(max_tokens);
  if (temperature >= 0) req.set_temperature(static_cast<float>(temperature));

  const auto channel = grpc::CreateChannel(target, grpc::InsecureChannelCredentials());
  const auto stub = LLM::NewStub(channel);
  grpc::ClientContext ctx;
  const auto reader = stub->Generate(&ctx, req);

  std::signal(SIGINT, on_sigint);

  GenerateEvent event;
  // ponytail: Ctrl-C is noticed between reads, so it lands within one token
  // (~45ms at 22 tok/s) or at the end of prefill. Good enough; a reader thread
  // would be the fix if that ever feels slow.
  while (g_interrupted == 0 && reader->Read(&event)) {
    if (event.has_delta()) {
      const std::string& text = event.delta().text();
      std::fwrite(text.data(), 1, text.size(), stdout);
      std::fflush(stdout);
    } else if (event.has_done()) {
      std::fputc('\n', stdout);
      if (!quiet) std::fprintf(stderr, "%s\n", stats(event.done()).c_str());
    }
  }

  if (g_interrupted != 0) {
    ctx.TryCancel();
    std::fputc('\n', stdout);
    reader->Finish();
    return 130;
  }

  const grpc::Status status = reader->Finish();
  if (!status.ok()) {
    if (status.error_code() == grpc::StatusCode::CANCELLED) return 130;
    std::fprintf(stderr, "llm: %s: %s\n", code_name(status.error_code()),
                 status.error_message().c_str());
    return 1;
  }
  return 0;
}
