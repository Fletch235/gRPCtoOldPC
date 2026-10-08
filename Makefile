# apt install build-essential libgrpc++-dev protobuf-compiler \
#             protobuf-compiler-grpc libcurl4-openssl-dev nlohmann-json3-dev
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
CXXFLAGS += -Igen $(shell pkg-config --cflags grpc++ protobuf libcurl)
# grpc++.pc does not list the reflection library, so name it explicitly.
LDLIBS   += -lgrpc++_reflection $(shell pkg-config --libs grpc++ protobuf libcurl)

GENSRC := gen/llm/v1/llm.pb.cc gen/llm/v1/llm.grpc.pb.cc
GENOBJ := $(GENSRC:.cc=.o)

all: bin/llmd bin/llm

$(GENSRC): llm/v1/llm.proto
	./generate.sh

%.o: %.cc
	$(CXX) $(CXXFLAGS) -c -o $@ $<

bin/%: src/%.cpp $(GENOBJ)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) -o $@ $< $(GENOBJ) $(LDLIBS)

clean:
	rm -rf bin gen

.PHONY: all clean
