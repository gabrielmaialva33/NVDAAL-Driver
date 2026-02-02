# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

NVDAAL (NVIDIA Ada Lovelace) is an open-source compute-only kernel extension (kext) for macOS that enables NVIDIA RTX 40 series GPUs for AI/ML workloads on Hackintosh systems. This is NOT a display driver - it focuses entirely on compute without framebuffer or display output support.

**Target**: macOS Tahoe 26+ via OpenCore 1.0.7+
**Language**: C++ (IOKit framework for kernel space)
**License**: MIT

## Build Commands

```bash
make                    # Build kext + tools + library
make clean              # Clean build artifacts
make rebuild            # Clean + build
make test               # Validate kext structure and Info.plist

# Installation and loading
make load               # Load kext temporarily (for testing, no reboot)
make unload             # Unload kext
make install            # Install to /Library/Extensions (requires reboot)
make reinstall          # Unload + clean + build + install

# Debugging
make logs               # View last 5 minutes of driver logs
make logs-live          # Stream driver logs in real-time (Ctrl+C to exit)
make status             # Check kext status and PCI device presence

# Firmware
make download-firmware  # Download GSP firmware from NVIDIA linux-firmware
```

## Architecture

```
User Space:
  [nvdaal-cli] / [Python Scripts]
         ↓
  [libNVDAAL.dylib] (C++ SDK)
         ↓
  [IOUserClient] (user<->kernel interface)
         ↓
Kernel Space:
  [NVDAAL.kext]
    ├─ NVDAALGsp (GSP controller, firmware, RPC)
    ├─ NVDAALMemory (VRAM allocator)
    ├─ NVDAALQueue (command submission)
    └─ NVDAALDisplay (fake display for Metal recognition)
         ↓
Hardware:
  [RTX 4090] - BAR0: MMIO / BAR1: VRAM
```

### Key Components

| File | Purpose |
|------|---------|
| `Sources/NVDAAL.cpp` | Main IOService: PCI probe, BAR mapping, lifecycle |
| `Sources/NVDAALGsp.cpp` | GSP controller: ELF parsing, boot sequence, RPC |
| `Sources/NVDAALUserClient.cpp` | IOUserClient: external method dispatcher |
| `Sources/NVDAALMemory.cpp` | Linear VRAM allocator |
| `Sources/NVDAALQueue.cpp` | Ring buffer command queue |
| `Sources/NVDAALRegs.h` | GPU register offsets + VBIOS structures (~650 lines) |
| `Library/libNVDAAL.cpp` | User-space C++ SDK wrapper |
| `Tools/nvdaal-cli/` | CLI firmware loader tool |

### GSP (GPU System Processor)

- Mandatory for Ada Lovelace (RTX 40 series)
- RISC-V core on GPU running firmware `gsp-570.144.bin`
- RPC protocol: signature 0x43505256, version 3
- Key functions: 0x15 (SYSTEM_INFO), 0x16 (SET_REGISTRY), 0x52 (INIT_DONE)

### VBIOS / FWSEC

- FWSEC-FRTS sets up WPR2 (Write Protected Region 2) for GSP
- VBIOS parsed for: ROM headers (0x55AA), PCIR, BIT table, PMU Lookup Table
- FWSEC image identified by PCIR codeType = 0xE0
- DMEMMAPPER patched with command 0x15 (FRTS) before Falcon execution

### Memory Layout

- **BAR0**: MMIO registers (16MB)
- **BAR1**: VRAM aperture (24GB on 4090)
- **GSP Heap**: 129MB (0x8100000)

## Coding Standards

- **Indentation**: 4 spaces (no tabs)
- **Braces**: Opening brace on same line
- **Naming**:
  - Classes: `PascalCase` with `NVDAAL` prefix (e.g., `NVDAALGsp`)
  - Methods: `camelCase` (e.g., `loadFirmware()`)
  - Constants: `UPPER_SNAKE_CASE` (e.g., `NV_PMC_BOOT_0`)
- **Logging**: Always prefix with `NVDAAL` or `NVDAAL-GSP`
  ```cpp
  IOLog("NVDAAL: Starting driver\n");
  IOLog("NVDAAL-GSP: RPC 0x%02x sent\n", function);
  ```
- **Header Guards**: `#ifndef NVDAAL_*_H` style
- **Error Handling**: Boolean returns, IOLog for errors, no exceptions (IOKit requirement)

## Supported Hardware

Device IDs (all Ada Lovelace AD1xx):
- RTX 4090: 0x2684, 0x2685
- RTX 4080: 0x2702, 0x2704
- RTX 4070 Ti: 0x2705, 0x2782
- RTX 4070: 0x2786, 0x2860

## Development Notes

- Use `make load` for temporary testing without reboot
- Enable live log streaming with `make logs-live` during development
- Boot argument required: `kext-dev-mode=1` or `amfi_get_out_of_my_way=0x1`
- macOS IOKit differs from Linux: use `IOBufferMemoryDescriptor` instead of `dma_alloc_coherent`, `IOUserClient` instead of `/dev` files

## Documentation

- `Docs/ARCHITECTURE.md` - Component design and register reference
- `Docs/GSP_INIT.md` - Detailed GSP boot sequence
- `Docs/TODO.md` - Development roadmap (Phases 1-6)
