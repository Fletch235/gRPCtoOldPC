// llmd exposes llama-server's chat completions as a single-flight gRPC service.
package main

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"log"
	"net/http"
	"strings"

	"connectrpc.com/connect"
	"connectrpc.com/grpcreflect"
	"golang.org/x/net/http2"
	"golang.org/x/net/http2/h2c"

	llmv1 "github.com/Fletch235/gRPCtoOldPC/gen/llm/v1"
	"github.com/Fletch235/gRPCtoOldPC/gen/llm/v1/llmv1connect"
)

// One generation at a time. llama-server runs with --parallel 1 and would queue
// anything extra invisibly and without bound, so the refusal happens here.
var slot = make(chan struct{}, 1)

type server struct {
	base string
	hc   *http.Client
}

var roleName = map[llmv1.Role]string{
	llmv1.Role_ROLE_SYSTEM:    "system",
	llmv1.Role_ROLE_USER:      "user",
	llmv1.Role_ROLE_ASSISTANT: "assistant",
}

var finishReason = map[string]llmv1.FinishReason{
	"stop":   llmv1.FinishReason_FINISH_REASON_STOP,
	"length": llmv1.FinishReason_FINISH_REASON_LENGTH,
}

func (s *server) Generate(
	ctx context.Context,
	req *connect.Request[llmv1.GenerateRequest],
	stream *connect.ServerStream[llmv1.GenerateEvent],
) error {
	up, err := upstreamRequest(req.Msg)
	if err != nil {
		return connect.NewError(connect.CodeInvalidArgument, err)
	}

	select {
	case slot <- struct{}{}:
		defer func() { <-slot }()
	default:
		return connect.NewError(connect.CodeResourceExhausted,
			errors.New("busy: one generation at a time"))
	}

	body, err := json.Marshal(up)
	if err != nil {
		return err
	}
	// ctx carries the client's cancellation, so a Ctrl-C on the client aborts the
	// HTTP request too and stops llama-server occupying the GPU.
	hreq, err := http.NewRequestWithContext(ctx, http.MethodPost,
		s.base+"/v1/chat/completions", bytes.NewReader(body))
	if err != nil {
		return err
	}
	hreq.Header.Set("Content-Type", "application/json")

	resp, err := s.hc.Do(hreq)
	if err != nil {
		if ctx.Err() != nil {
			return ctx.Err()
		}
		return connect.NewError(connect.CodeUnavailable, fmt.Errorf("llama-server: %w", err))
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		msg, _ := io.ReadAll(io.LimitReader(resp.Body, 4<<10))
		return connect.NewError(connect.CodeUnavailable,
			fmt.Errorf("llama-server %s: %s", resp.Status, strings.TrimSpace(string(msg))))
	}

	done := &llmv1.Done{}
	sc := bufio.NewScanner(resp.Body)
	sc.Buffer(make([]byte, 0, 64<<10), 1<<20)
	for sc.Scan() {
		data, ok := strings.CutPrefix(strings.TrimSpace(sc.Text()), "data:")
		if !ok {
			continue // blank separator line, or an SSE field we don't use
		}
		data = strings.TrimSpace(data)
		if data == "[DONE]" {
			break
		}
		var chunk upChunk
		if err := json.Unmarshal([]byte(data), &chunk); err != nil {
			continue
		}
		// Counts and timings arrive on the trailing chunk; keep the last seen so a
		// build that omits them just leaves zeros rather than failing the stream.
		if u := chunk.Usage; u != nil {
			done.PromptTokens, done.CompletionTokens = u.PromptTokens, u.CompletionTokens
		}
		if t := chunk.Timings; t != nil {
			done.PrefillMs, done.GenerateMs = int64(t.PromptMS), int64(t.PredictedMS)
		}
		for _, c := range chunk.Choices {
			if r, ok := finishReason[c.FinishReason]; ok {
				done.FinishReason = r
			}
			if c.Delta.Content == "" {
				continue
			}
			err := stream.Send(&llmv1.GenerateEvent{
				Event: &llmv1.GenerateEvent_Delta{Delta: &llmv1.Delta{Text: c.Delta.Content}},
			})
			if err != nil {
				return err
			}
		}
	}
	if err := sc.Err(); err != nil {
		if ctx.Err() != nil {
			return ctx.Err()
		}
		return connect.NewError(connect.CodeUnavailable, err)
	}
	return stream.Send(&llmv1.GenerateEvent{Event: &llmv1.GenerateEvent_Done{Done: done}})
}

func upstreamRequest(req *llmv1.GenerateRequest) (*upRequest, error) {
	if len(req.GetMessages()) == 0 {
		return nil, errors.New("messages must not be empty")
	}
	up := &upRequest{
		Stream:        true,
		StreamOptions: &streamOptions{IncludeUsage: true},
		MaxTokens:     req.MaxTokens,
		Temperature:   req.Temperature,
		TopP:          req.TopP,
		Seed:          req.Seed,
		Stop:          req.GetStop(),
	}
	for i, m := range req.GetMessages() {
		role, ok := roleName[m.GetRole()]
		if !ok {
			return nil, fmt.Errorf("messages[%d]: role is %v", i, m.GetRole())
		}
		up.Messages = append(up.Messages, upMessage{Role: role, Content: m.GetContent()})
	}
	return up, nil
}

type upRequest struct {
	Messages      []upMessage    `json:"messages"`
	Stream        bool           `json:"stream"`
	StreamOptions *streamOptions `json:"stream_options,omitempty"`
	MaxTokens     *int32         `json:"max_tokens,omitempty"`
	Temperature   *float32       `json:"temperature,omitempty"`
	TopP          *float32       `json:"top_p,omitempty"`
	Seed          *uint64        `json:"seed,omitempty"`
	Stop          []string       `json:"stop,omitempty"`
}

type streamOptions struct {
	IncludeUsage bool `json:"include_usage"`
}

type upChunk struct {
	Choices []struct {
		Delta struct {
			Content string `json:"content"`
		} `json:"delta"`
		FinishReason string `json:"finish_reason"`
	} `json:"choices"`
	Usage *struct {
		PromptTokens     int32 `json:"prompt_tokens"`
		CompletionTokens int32 `json:"completion_tokens"`
	} `json:"usage"`
	Timings *struct {
		PromptMS    float64 `json:"prompt_ms"`
		PredictedMS float64 `json:"predicted_ms"`
	} `json:"timings"`
}

func main() {
	addr := flag.String("addr", ":50051", "address to serve gRPC on")
	llama := flag.String("llama", "http://127.0.0.1:8080", "llama-server base URL")
	flag.Parse()

	// No client timeout: a generation legitimately streams for minutes.
	s := &server{base: strings.TrimRight(*llama, "/"), hc: &http.Client{}}

	mux := http.NewServeMux()
	mux.Handle(llmv1connect.NewLLMHandler(s))
	reflector := grpcreflect.NewStaticReflector(llmv1connect.LLMName)
	mux.Handle(grpcreflect.NewHandlerV1(reflector))
	mux.Handle(grpcreflect.NewHandlerV1Alpha(reflector))

	log.Printf("llmd on %s, upstream %s", *addr, s.base)
	// h2c so native gRPC clients (grpcurl -plaintext) work without TLS.
	log.Fatal(http.ListenAndServe(*addr, h2c.NewHandler(mux, &http2.Server{})))
}
