// llmd exposes llama-server's chat completions as a single-flight gRPC service.
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/grpcpp.h>

#include "llm/v1/llm.grpc.pb.h"

using json = nlohmann::json;
using namespace llm::v1;

namespace {

// nlohmann's value() throws when a key is present but null, which an
// OpenAI-shaped stream does for finish_reason on every non-final chunk.
std::string str_field(const json& o, const char* key) {
  const auto it = o.find(key);
  return (it != o.end() && it->is_string()) ? it->get<std::string>() : std::string{};
}

double num_field(const json& o, const char* key) {
  const auto it = o.find(key);
  return (it != o.end() && it->is_number()) ? it->get<double>() : 0.0;
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
  return s;
}

const char* role_name(Role r) {
  switch (r) {
    case ROLE_SYSTEM: return "system";
    case ROLE_USER: return "user";
    case ROLE_ASSISTANT: return "assistant";
    default: return nullptr;
  }
}

// Returns an error message, or empty on success.
std::string build_upstream_request(const GenerateRequest& req, std::string* out) {
  if (req.messages_size() == 0) return "messages must not be empty";

  json messages = json::array();
  for (int i = 0; i < req.messages_size(); ++i) {
    const char* role = role_name(req.messages(i).role());
    if (role == nullptr) return "messages[" + std::to_string(i) + "]: role is not set";
    messages.push_back({{"role", role}, {"content", req.messages(i).content()}});
  }

  json body = {
      {"messages", std::move(messages)},
      {"stream", true},
      {"stream_options", {{"include_usage", true}}},
  };
  if (req.has_max_tokens()) body["max_tokens"] = req.max_tokens();
  if (req.has_temperature()) body["temperature"] = req.temperature();
  if (req.has_top_p()) body["top_p"] = req.top_p();
  if (req.has_seed()) body["seed"] = req.seed();
  if (req.stop_size() > 0) {
    body["stop"] = std::vector<std::string>(req.stop().begin(), req.stop().end());
  }
  *out = body.dump();
  return {};
}

struct StreamState {
  grpc::ServerContext* ctx = nullptr;
  grpc::ServerWriter<GenerateEvent>* writer = nullptr;

  std::string pending;  // bytes of an SSE line not yet terminated by a newline
  std::string raw;      // first 4 KiB, only read back to report a non-200 body
  Done done;

  // Why the write callback stopped the transfer, so the status code can say.
  bool saw_done = false;
  bool cancelled = false;
  bool client_gone = false;
};

// Returns false to stop the transfer.
bool handle_sse_line(StreamState* st, std::string_view line) {
  if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

  constexpr std::string_view kData = "data:";
  if (line.substr(0, kData.size()) != kData) {
    return true;  // a blank separator line, or an SSE field we don't use
  }
  const std::string_view payload = trim(line.substr(kData.size()));
  if (payload == "[DONE]") {
    st->saw_done = true;
    return false;
  }

  const json chunk = json::parse(payload, nullptr, /*allow_exceptions=*/false);
  if (chunk.is_discarded() || !chunk.is_object()) return true;

  // Counts and timings arrive on the trailing chunk. Keep the last seen, so a
  // build that omits them leaves zeros rather than failing the stream.
  if (const auto u = chunk.find("usage"); u != chunk.end() && u->is_object()) {
    st->done.set_prompt_tokens(static_cast<int32_t>(num_field(*u, "prompt_tokens")));
    st->done.set_completion_tokens(static_cast<int32_t>(num_field(*u, "completion_tokens")));
  }
  if (const auto t = chunk.find("timings"); t != chunk.end() && t->is_object()) {
    st->done.set_prefill_ms(static_cast<int64_t>(num_field(*t, "prompt_ms")));
    st->done.set_generate_ms(static_cast<int64_t>(num_field(*t, "predicted_ms")));
  }

  const auto choices = chunk.find("choices");
  if (choices == chunk.end() || !choices->is_array()) return true;
  for (const json& c : *choices) {
    if (!c.is_object()) continue;
    const std::string reason = str_field(c, "finish_reason");
    if (reason == "stop") {
      st->done.set_finish_reason(FINISH_REASON_STOP);
    } else if (reason == "length") {
      st->done.set_finish_reason(FINISH_REASON_LENGTH);
    }

    const auto delta = c.find("delta");
    if (delta == c.end() || !delta->is_object()) continue;
    const std::string text = str_field(*delta, "content");
    if (text.empty()) continue;

    GenerateEvent event;
    event.mutable_delta()->set_text(text);
    if (!st->writer->Write(event)) {
      st->client_gone = true;
      return false;
    }
  }
  return true;
}

size_t on_data(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* st = static_cast<StreamState*>(userdata);
  const size_t n = size * nmemb;

  // Returning anything other than n aborts the transfer, which is how an
  // abandoned generation stops occupying the GPU.
  if (st->ctx->IsCancelled()) {
    st->cancelled = true;
    return 0;
  }

  if (st->raw.size() < 4096) {
    st->raw.append(ptr, std::min(n, size_t{4096} - st->raw.size()));
  }
  st->pending.append(ptr, n);
  for (size_t nl; (nl = st->pending.find('\n')) != std::string::npos;) {
    const bool keep_going = handle_sse_line(st, std::string_view(st->pending).substr(0, nl));
    st->pending.erase(0, nl + 1);
    if (!keep_going) return 0;
  }
  return n;
}

class LLMService final : public LLM::Service {
 public:
  explicit LLMService(std::string base) : base_(std::move(base)) {}

  grpc::Status Generate(grpc::ServerContext* ctx, const GenerateRequest* req,
                        grpc::ServerWriter<GenerateEvent>* writer) override {
    std::string body;
    if (const std::string err = build_upstream_request(*req, &body); !err.empty()) {
      return {grpc::StatusCode::INVALID_ARGUMENT, err};
    }

    // One generation at a time. llama-server runs with --parallel 1 and would
    // queue anything extra invisibly and without bound, so refuse here instead.
    std::unique_lock<std::mutex> slot(slot_, std::try_to_lock);
    if (!slot.owns_lock()) {
      return {grpc::StatusCode::RESOURCE_EXHAUSTED, "busy: one generation at a time"};
    }

    CURL* curl = curl_easy_init();
    if (curl == nullptr) return {grpc::StatusCode::INTERNAL, "curl_easy_init failed"};

    StreamState st;
    st.ctx = ctx;
    st.writer = writer;

    const std::string url = base_ + "/v1/chat/completions";
    char errbuf[CURL_ERROR_SIZE] = {};
    curl_slist* headers = curl_slist_append(nullptr, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &st);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    // No total timeout: a generation legitimately streams for minutes.
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);  // we are off the main thread

    const CURLcode rc = curl_easy_perform(curl);
    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (st.cancelled || ctx->IsCancelled() || st.client_gone) {
      return grpc::Status::CANCELLED;
    }
    if (http_status != 0 && http_status != 200) {
      const std::string_view detail = trim(st.raw);
      return {grpc::StatusCode::UNAVAILABLE,
              "llama-server HTTP " + std::to_string(http_status) + ": " + std::string(detail)};
    }
    if (rc != CURLE_OK && !st.saw_done) {
      return {grpc::StatusCode::UNAVAILABLE,
              std::string("llama-server: ") +
                  (errbuf[0] != '\0' ? errbuf : curl_easy_strerror(rc))};
    }

    GenerateEvent event;
    *event.mutable_done() = st.done;
    writer->Write(event);
    return grpc::Status::OK;
  }

 private:
  std::string base_;
  std::mutex slot_;
};

}  // namespace

int main(int argc, char** argv) {
  // Loopback by default: nothing is exposed off-host until TLS and auth land.
  std::string addr = "127.0.0.1:50051";
  std::string llama = "http://127.0.0.1:8080";
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if ((arg == "--addr" || arg == "-addr") && i + 1 < argc) {
      addr = argv[++i];
    } else if ((arg == "--llama" || arg == "-llama") && i + 1 < argc) {
      llama = argv[++i];
    } else {
      std::fprintf(stderr, "usage: llmd [--addr HOST:PORT] [--llama URL]\n");
      return 2;
    }
  }
  while (!llama.empty() && llama.back() == '/') llama.pop_back();

  curl_global_init(CURL_GLOBAL_DEFAULT);
  grpc::reflection::InitProtoReflectionServerBuilderPlugin();

  LLMService service(llama);
  grpc::ServerBuilder builder;
  builder.AddListeningPort(addr, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);

  const std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
  if (!server) {
    std::fprintf(stderr, "llmd: could not bind %s\n", addr.c_str());
    return 1;
  }
  std::fprintf(stderr, "llmd on %s, upstream %s\n", addr.c_str(), llama.c_str());
  server->Wait();
  return 0;
}
