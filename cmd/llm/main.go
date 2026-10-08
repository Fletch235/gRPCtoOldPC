// llm sends one prompt to llmd and streams the answer to stdout.
package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"io"
	"net/http"
	"os"
	"os/signal"
	"strings"

	"connectrpc.com/connect"
	"google.golang.org/protobuf/proto"

	llmv1 "github.com/Fletch235/gRPCtoOldPC/gen/llm/v1"
	"github.com/Fletch235/gRPCtoOldPC/gen/llm/v1/llmv1connect"
)

func main() {
	addr := flag.String("s", envOr("LLM_ADDR", "http://localhost:50051"), "llmd base URL")
	system := flag.String("system", "", "system prompt")
	maxTokens := flag.Int("n", 0, "max tokens (0 leaves it to the server)")
	temp := flag.Float64("temp", -1, "temperature (negative leaves it to the server)")
	quiet := flag.Bool("q", false, "omit the stats line on stderr")
	flag.Parse()

	prompt := strings.TrimSpace(strings.Join(flag.Args(), " "))
	if prompt == "" {
		b, err := io.ReadAll(os.Stdin)
		if err != nil {
			fatal(err)
		}
		prompt = strings.TrimSpace(string(b))
	}
	if prompt == "" {
		fmt.Fprintln(os.Stderr, "usage: llm [flags] prompt...   (or pipe the prompt on stdin)")
		flag.PrintDefaults()
		os.Exit(2)
	}

	req := &llmv1.GenerateRequest{}
	if *system != "" {
		req.Messages = append(req.Messages, &llmv1.Message{
			Role: llmv1.Role_ROLE_SYSTEM, Content: *system,
		})
	}
	req.Messages = append(req.Messages, &llmv1.Message{
		Role: llmv1.Role_ROLE_USER, Content: prompt,
	})
	if *maxTokens > 0 {
		req.MaxTokens = proto.Int32(int32(*maxTokens))
	}
	if *temp >= 0 {
		req.Temperature = proto.Float32(float32(*temp))
	}

	// Ctrl-C cancels the RPC, which cancels llmd's request to llama-server.
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt)
	defer stop()

	// Default Connect protocol over HTTP/1.1 — no h2c transport needed here.
	client := llmv1connect.NewLLMClient(&http.Client{}, strings.TrimRight(*addr, "/"))
	stream, err := client.Generate(ctx, connect.NewRequest(req))
	if err != nil {
		fatal(err)
	}
	defer stream.Close()

	for stream.Receive() {
		switch e := stream.Msg().GetEvent().(type) {
		case *llmv1.GenerateEvent_Delta:
			os.Stdout.WriteString(e.Delta.GetText())
		case *llmv1.GenerateEvent_Done:
			fmt.Println()
			if !*quiet {
				fmt.Fprintln(os.Stderr, stats(e.Done))
			}
		}
	}
	if err := stream.Err(); err != nil {
		if errors.Is(err, context.Canceled) || connect.CodeOf(err) == connect.CodeCanceled {
			fmt.Println()
			os.Exit(130)
		}
		fatal(err)
	}
}

func stats(d *llmv1.Done) string {
	rate := ""
	if ms := d.GetGenerateMs(); ms > 0 {
		rate = fmt.Sprintf(" (%.1f tok/s)", float64(d.GetCompletionTokens())*1000/float64(ms))
	}
	return fmt.Sprintf("%s: %d prompt + %d completion tokens, prefill %dms, generate %dms%s",
		strings.ToLower(strings.TrimPrefix(d.GetFinishReason().String(), "FINISH_REASON_")),
		d.GetPromptTokens(), d.GetCompletionTokens(),
		d.GetPrefillMs(), d.GetGenerateMs(), rate)
}

func envOr(key, def string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return def
}

// Connect errors already stringify as "<code>: <message>".
func fatal(err error) {
	fmt.Fprintln(os.Stderr, "llm:", err)
	os.Exit(1)
}
