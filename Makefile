# RX 7900 XTX VRAM test
#
#   make run
#   make run ROUNDS=10 CHUNK_MIB=256
#   make clean
#
# Each run writes vram-test-YYYYMMDD-HHMMSS.json in the current directory.
# The file is replaced after every round. make clean leaves that log in place.
#
#   make view
#   make view LOG=vram-test-YYYYMMDD-HHMMSS.json

ROCM  ?= /opt/rocm
CXX   ?= g++

ROUNDS    ?= 10
CHUNK_MIB ?= 256

TARGET := vram-test
VIEW   := vram-view
LOG    ?=

CXXFLAGS := -O2 -std=c++17 -Wall -Wextra -D__HIP_PLATFORM_AMD__ -I$(ROCM)/include -pthread
LDFLAGS  := -L$(ROCM)/lib -lamdhip64 -Wl,-rpath,$(ROCM)/lib -pthread

.PHONY: build run clean view

.DEFAULT_GOAL := run

build: $(TARGET) $(VIEW)

$(TARGET): vram_test.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

$(VIEW): vram_view.cpp
	$(CXX) -O2 -std=c++17 -Wall -Wextra -o $@ $<

run: $(TARGET)
	@echo "=== run: ROUNDS=$(ROUNDS) CHUNK_MIB=$(CHUNK_MIB) ==="
	./$(TARGET) $(ROUNDS) $(CHUNK_MIB)

view: $(VIEW)
	./$(VIEW) $(LOG)

clean:
	rm -f $(TARGET) $(VIEW) gpucore.* vram-test-*.json.tmp
