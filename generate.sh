#!/bin/sh
# Regenerate gen/ from llm/v1/llm.proto.
# Needs: apt install protobuf-compiler, plus these two plugins on PATH:
#   go install google.golang.org/protobuf/cmd/protoc-gen-go@latest
#   go install connectrpc.com/connect/cmd/protoc-gen-connect-go@latest
set -e
mod=github.com/Fletch235/gRPCtoOldPC/gen
mkdir -p gen
protoc \
  --go_out=gen --go_opt=module=$mod \
  --connect-go_out=gen --connect-go_opt=module=$mod \
  llm/v1/llm.proto
