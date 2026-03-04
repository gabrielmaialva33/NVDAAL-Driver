CC = gcc
CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -g
CFLAGS = -std=c11 -Wall -Wextra -O2 -g
INCLUDES = -I../../Sources

# All test binaries
TESTS = test_vbios_parse test_gsp_firmware test_rpc_structs test_register_read

.PHONY: all clean test

all: $(TESTS)

test_vbios_parse: test_vbios_parse.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $<

test_gsp_firmware: test_gsp_firmware.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $<

test_rpc_structs: test_rpc_structs.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $<

test_register_read: test_register_read.c
	$(CXX) $(CXXFLAGS) $(INCLUDES) -x c++ -o $@ $<

test: all
	@echo "=== Running VBIOS Parser Test ==="
	./test_vbios_parse ../ReverseEng/vbios_full.rom
	@echo ""
	@echo "=== Running GSP Firmware Test ==="
	./test_gsp_firmware ../ReverseEng/gsp-570.144.bin
	@echo ""
	@echo "=== Running RPC Struct Test ==="
	./test_rpc_structs
	@echo ""
	@echo "=== Register Read (needs sudo) ==="
	@echo "Run: sudo ./test_register_read"

clean:
	rm -f $(TESTS) *.o
