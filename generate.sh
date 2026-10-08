#!/bin/sh
# Regenerate gen/ from llm/v1/llm.proto.
# Needs: apt install protobuf-compiler protobuf-compiler-grpc
set -e
mkdir -p gen
protoc -I. \
  --cpp_out=gen \
  --grpc_out=gen --plugin=protoc-gen-grpc="$(command -v grpc_cpp_plugin)" \
  llm/v1/llm.proto
