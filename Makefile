# RX 7900 XTX VRAM test
#
#   make run
#   make run ROUNDS=10 CHUNK_MIB=256
#   make clean

ROCM  ?= /opt/rocm
CXX   ?= g++

ROUNDS    ?= 10
CHUNK_MIB ?= 256

TARGET := vram-test

CXXFLAGS := -O2 -std=c++17 -Wall -Wextra -D__HIP_PLATFORM_AMD__ -I$(ROCM)/include -pthread
LDFLAGS  := -L$(ROCM)/lib -lamdhip64 -Wl,-rpath,$(ROCM)/lib -pthread

.PHONY: build run clean

.DEFAULT_GOAL := run

build: $(TARGET)

$(TARGET): vram_test.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

run: build
	@echo "=== run: ROUNDS=$(ROUNDS) CHUNK_MIB=$(CHUNK_MIB) ==="
	./$(TARGET) $(ROUNDS) $(CHUNK_MIB)

clean:
	rm -f $(TARGET) gpucore.*
