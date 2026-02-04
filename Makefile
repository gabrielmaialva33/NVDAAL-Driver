# Makefile for NVDAAL.kext
# NVIDIA Ada Lovelace Compute Driver for macOS
# Focus: AI/ML - No display support

KEXT_NAME = NVDAAL
BUNDLE_ID = com.nvdaal.compute
BUILD_DIR = Build
KEXT_PATH = $(BUILD_DIR)/$(KEXT_NAME).kext
INFO_PLIST = Info.plist

# Source files
SOURCES = Sources/NVDAAL.cpp Sources/NVDAALGsp.cpp Sources/NVDAALUserClient.cpp Sources/NVDAALMemory.cpp Sources/NVDAALVASpace.cpp Sources/NVDAALChannel.cpp Sources/NVDAALDisplay.cpp

# Object files
OBJECTS = $(BUILD_DIR)/NVDAAL.o $(BUILD_DIR)/NVDAALGsp.o $(BUILD_DIR)/NVDAALUserClient.o $(BUILD_DIR)/NVDAALMemory.o $(BUILD_DIR)/NVDAALVASpace.o $(BUILD_DIR)/NVDAALChannel.o $(BUILD_DIR)/NVDAALDisplay.o

# Compiler and Flags
SDKROOT ?= $(shell xcrun --sdk macosx --show-sdk-path)
ARCH ?= $(shell uname -m)
CXX = xcrun -sdk macosx clang++
CXXFLAGS = -x c++ -std=c++17 -arch $(ARCH) -fno-builtin -fno-exceptions -fno-rtti -mkernel \
	-DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
	-Wno-deprecated-declarations -Wno-inconsistent-missing-override -Wno-shadow \
	-I$(SDKROOT)/System/Library/Frameworks/Kernel.framework/Headers \
	-I$(SDKROOT)/System/Library/Frameworks/IOKit.framework/Headers \
	-I./Sources

LDFLAGS = -arch $(ARCH) -static -Xlinker -kext -nostdlib -lkmod -lcc_kext

# =============================================================================
# Targets
# =============================================================================

all: $(KEXT_PATH) tools lib
	@echo "[+] Build complete: $(KEXT_PATH), Tools & Library"

lib: $(BUILD_DIR)/libNVDAAL.dylib

$(BUILD_DIR)/libNVDAAL.dylib: Library/libNVDAAL.cpp Library/nvdaal_c_api.cpp Library/libNVDAAL.h
	@mkdir -p $(BUILD_DIR)
	clang++ -dynamiclib -std=c++17 -framework IOKit -framework CoreFoundation -I./Library \
		-install_name @rpath/libNVDAAL.dylib \
		Library/libNVDAAL.cpp Library/nvdaal_c_api.cpp -o $@
	@echo "[*] Shared Library: $@"

tools:
	@echo "[*] Building tools..."
	$(MAKE) -C Tools/nvdaal-cli

$(KEXT_PATH): $(BUILD_DIR) $(KEXT_PATH)/Contents/MacOS/$(KEXT_NAME) $(KEXT_PATH)/Contents/Info.plist

$(BUILD_DIR)/NVDAAL.o: Sources/NVDAAL.cpp Sources/NVDAALRegs.h Sources/NVDAALGsp.h
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR)/NVDAALGsp.o: Sources/NVDAALGsp.cpp Sources/NVDAALGsp.h Sources/NVDAALRegs.h
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR)/NVDAALUserClient.o: Sources/NVDAALUserClient.cpp Sources/NVDAALUserClient.h Sources/NVDAAL.h
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR)/NVDAALMemory.o: Sources/NVDAALMemory.cpp Sources/NVDAALMemory.h
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR)/NVDAALVASpace.o: Sources/NVDAALVASpace.cpp Sources/NVDAALVASpace.h Sources/NVDAALRegs.h
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR)/NVDAALChannel.o: Sources/NVDAALChannel.cpp Sources/NVDAALChannel.h Sources/NVDAALRegs.h
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR)/NVDAALDisplay.o: Sources/NVDAALDisplay.cpp Sources/NVDAALDisplay.h
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(KEXT_PATH)/Contents/MacOS/$(KEXT_NAME): $(OBJECTS)
	@mkdir -p $(dir $@)
	$(CXX) $(LDFLAGS) -o $@ $(OBJECTS)
	@echo "[*] Binary: $@"
	@echo "[*] Signing kext (ad-hoc)..."
	@codesign -s - --force --deep $(KEXT_PATH)/Contents/MacOS/$(KEXT_NAME)

$(KEXT_PATH)/Contents/Info.plist: $(INFO_PLIST)
	@mkdir -p $(dir $@)
	cp $< $@

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

# =============================================================================
# Development Commands
# =============================================================================

clean:
	rm -rf $(BUILD_DIR)
	@echo "[*] Build cleaned."

rebuild: clean all

# Verify kext structure
check-kext: $(KEXT_PATH)
	@echo "[*] Verifying kext structure..."
	@ls -la $(KEXT_PATH)/Contents/
	@ls -la $(KEXT_PATH)/Contents/MacOS/
	@echo "[*] Validating Info.plist..."
	@plutil $(KEXT_PATH)/Contents/Info.plist
	@echo "[*] Checking binary..."
	@file $(KEXT_PATH)/Contents/MacOS/$(KEXT_NAME)
	@echo "[+] Structure OK!"

# =============================================================================
# Tests
# =============================================================================

TEST_DIR = Tests

# Compile all tests
test: test-structures test-vbios-real test-library test-driver
	@echo "[+] All tests compiled!"
	@echo "[*] To run: make run-tests"

# Run all tests
run-tests: test
	@echo "\n=========================================="
	@echo "  NVDAAL Test Suite"
	@echo "=========================================="
	@echo "\n[1/4] Structure tests..."
	@./$(BUILD_DIR)/test_structures || true
	@echo "\n[2/4] VBIOS real tests..."
	@./$(BUILD_DIR)/test_vbios_real || true
	@echo "\n[3/4] Library tests..."
	@./$(BUILD_DIR)/test_library || true
	@echo "\n[4/4] Driver tests..."
	@./$(BUILD_DIR)/test_driver || true
	@echo "\n=========================================="
	@echo "  Tests completed!"
	@echo "=========================================="

# Structure tests (no hardware required)
test-structures: $(BUILD_DIR)/test_structures
$(BUILD_DIR)/test_structures: $(TEST_DIR)/test_structures.c $(TEST_DIR)/nvdaal_test.h Sources/NVDAALRegs.h
	@mkdir -p $(BUILD_DIR)
	clang -std=c11 -Wall -Wextra -I$(TEST_DIR) -I./Sources -o $@ $(TEST_DIR)/test_structures.c
	@echo "[*] Compiled: $@"

# VBIOS real tests (requires Firmware/AD102.rom)
test-vbios-real: $(BUILD_DIR)/test_vbios_real
$(BUILD_DIR)/test_vbios_real: $(TEST_DIR)/test_vbios_real.c $(TEST_DIR)/nvdaal_test.h Sources/NVDAALRegs.h
	@mkdir -p $(BUILD_DIR)
	clang -std=c11 -Wall -Wextra -I$(TEST_DIR) -I./Sources -o $@ $(TEST_DIR)/test_vbios_real.c
	@echo "[*] Compiled: $@"

# Library tests (requires Build/libNVDAAL.dylib)
test-library: lib $(BUILD_DIR)/test_library
$(BUILD_DIR)/test_library: $(TEST_DIR)/test_library.c $(TEST_DIR)/nvdaal_test.h
	@mkdir -p $(BUILD_DIR)
	clang -std=c11 -Wall -Wextra -I$(TEST_DIR) -o $@ $(TEST_DIR)/test_library.c
	@echo "[*] Compiled: $@"

# Driver tests (requires kext loaded)
test-driver: $(BUILD_DIR)/test_driver
$(BUILD_DIR)/test_driver: $(TEST_DIR)/test_driver.c $(TEST_DIR)/nvdaal_test.h
	@mkdir -p $(BUILD_DIR)
	clang -std=c11 -Wall -Wextra -I$(TEST_DIR) -framework IOKit -framework CoreFoundation \
		-o $@ $(TEST_DIR)/test_driver.c
	@echo "[*] Compiled: $@"

# Quick test (structures only - no hardware required)
test-quick: test-structures
	@./$(BUILD_DIR)/test_structures

# Test specific VBIOS
test-vbios: test-vbios-real
	@./$(BUILD_DIR)/test_vbios_real Firmware/AD102.rom

# Clean tests
clean-tests:
	rm -f $(BUILD_DIR)/test_*
	@echo "[*] Tests cleaned."

# =============================================================================
# Installation and Management
# =============================================================================

# Install permanently (requires reboot)
install: $(KEXT_PATH)
	@echo "[*] Installing NVDAAL.kext..."
	sudo chown -R root:wheel $(KEXT_PATH)
	sudo cp -R $(KEXT_PATH) /Library/Extensions/
	sudo touch /Library/Extensions/
	sudo kextcache -invalidate /
	@echo "[+] Kext installed. Reboot required."
	@echo "[!] Make sure you have boot-args: kext-dev-mode=1"

# Load temporarily (for testing)
load: $(KEXT_PATH)
	@echo "[*] Loading NVDAAL.kext..."
	sudo chown -R root:wheel $(KEXT_PATH)
	sudo kextload $(KEXT_PATH)
	@echo "[+] Kext loaded."
	@echo "[*] Check logs: make logs"

# Unload
unload:
	@echo "[*] Unloading NVDAAL.kext..."
	-sudo kextunload -b $(BUNDLE_ID)
	@echo "[+] Kext unloaded."

# Reinstall (unload + install)
reinstall: unload clean all install

# =============================================================================
# Debug and Logs
# =============================================================================

# View driver logs
logs:
	@echo "[*] NVDAAL logs (last 5 minutes):"
	log show --predicate 'eventMessage contains "NVDAAL"' --last 5m

# View logs in real-time
logs-live:
	@echo "[*] NVDAAL logs (real-time - Ctrl+C to exit):"
	log stream --predicate 'eventMessage contains "NVDAAL"'

# Kext status
status:
	@echo "[*] NVDAAL Status:"
	@kextstat | grep -i nvdaal || echo "Kext not loaded"
	@echo ""
	@echo "[*] NVIDIA PCI Devices:"
	@ioreg -l | grep -i \"class IOPCIDevice\" -A 20 | grep -i nvidia || echo "None found"

# =============================================================================
# Firmware
# =============================================================================

# Download GSP firmware
download-firmware:
	@echo "[*] Downloading GSP firmware..."
	@mkdir -p Firmware
	curl -L -o Firmware/gsp-570.144.bin \
		"https://github.com/NVIDIA/linux-firmware/raw/nvidia-staging/nvidia/ad102/gsp/gsp-570.144.bin" || \
	curl -L -o Firmware/gsp-570.144.bin \
		"https://github.com/NVIDIA/linux-firmware/raw/refs/heads/nvidia-staging/nvidia/ga102/gsp/gsp-570.144.bin"
	@ls -la Firmware/gsp-570.144.bin
	@echo "[+] Firmware downloaded."

.PHONY: all clean rebuild test test-quick test-vbios test-structures test-vbios-real test-library test-driver \
       run-tests clean-tests check-kext install load unload reinstall logs logs-live status download-firmware
