# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.4.0] - 2026-02-01 - Enhanced Boot

### Added
- **Complete Ada Lovelace Boot Sequence**
  - SEC2 FALCON reset and booter_load execution
  - FWSEC-FRTS for WPR2 region setup
  - Detailed boot stage error reporting (`bootEx()`)
  - Pre/post boot state diagnostics (RISCV_CTL, BR_RETCODE, SCRATCH14)
- **New Register Definitions** (NVDAALRegs.h)
  - SEC2 FALCON and RISC-V registers
  - WPR2 address registers (NV_PFB_PRI_MMU_WPR2_*)
  - FWSEC error register (NV_PBUS_VBIOS_SCRATCH_FWSEC_ERR)
  - VBIOS ROM structures and constants
  - Falcon DMA transfer commands
- **New Firmware Loading APIs**
  - `loadBooterLoad()` - SEC2 booter firmware
  - `loadVbios()` - VBIOS for FWSEC extraction
  - `loadGspFirmwareEx()` - Returns error stage instead of bool
- **IOUserClient Enhancements**
  - `methodLoadBooterLoad()` - SEC2 booter upload (max 1MB)
  - `methodLoadVbios()` - VBIOS upload (max 4MB)
  - Detailed error codes (0xe0000300 + stage)
- **libNVDAAL SDK Extensions**
  - `loadBooterLoad(path)` / `loadBooterLoad(data, size)`
  - `loadVbios(path)` / `loadVbios(data, size)`
  - Error logging on firmware load failure
- **nvdaal-cli `boot` Command**
  - Full boot sequence with automatic firmware discovery
  - Expects: gsp-570.144.bin, booter_load-ad102-570.144.bin, AD102.rom
  - Graceful fallback when optional firmwares missing

### Changed
- **Non-Contiguous Firmware Allocation**
  - Removed kIOMemoryPhysicallyContiguous for 63MB GSP-RM
  - Per-page physical address lookup via `getPhysicalSegment()`
  - Radix3 table builder handles scatter-gather pages
- **Bundle Identifier**: `com.nvidia.nvdaal` → `com.nvdaal.compute`
- **Makefile Multi-Architecture Support**
  - Auto-detect arch via `uname -m` (ARCH variable)
  - Fixed linker flags: `-static -lcc_kext`
- Added `com.apple.iokit.IOPCIFamily 2.9` dependency
- Added `KMOD_EXPLICIT_DECL` for modern macOS kext loading (kmutil/AuxKC)

### Fixed
- Large firmware allocation failures on systems without 63MB contiguous memory
- **RISC-V register base address** for Ada Lovelace (0x110000 → 0x118000)
  - GSP RISC-V registers are at 0x118000, not 0x110000
  - 0x110000 is for GSP Falcon interface only

### Debug
- **Debug mode**: Boot continues even when FWSEC-FRTS or booter_load fail
- **Register scanning**: Probes multiple base addresses to find RISC-V registers
- **Enhanced diagnostics**: Pre/post boot state, periodic status, final state dump

## [0.3.1] - 2026-01-30 - MMU Testing

### Added
- MMU testing infrastructure
- Interrupt handling improvements

### Fixed
- Minor stability fixes

## [0.3.0] - 2026-01-29 - Pioneer

### Added
- **Full Compute Engine Implementation**
  - GPFIFO Channel creation
  - User Doorbell (UserD) mapping
  - Command submission infrastructure
- **Complete RPC Engine**
  - rmAlloc / rmControl implementations
  - Lock-free queue management
- **Interrupt Driven Architecture**
  - MSI (Message Signaled Interrupts) support
  - Reactive status queue processing
- **Memory Management (MMU)**
  - Virtual Address Space (VASpace)
  - Page Directory/Table management
  - Bump allocator for VRAM
- **User-Space Interface**
  - IOUserClient for secure firmware upload
  - Zero-copy memory mapping
  - libNVDAAL.dylib shared library
- **CLI Tool** (nvdaal-cli)
  - `load` command for firmware upload
  - `test` command for VRAM validation

### Changed
- Refactored GSP boot sequence
- Improved ELF parser for GSP firmware

## [0.2.0] - 2026-01-28 - GSP Foundation

### Added
- GSP Controller (NVDAALGsp)
  - ELF firmware parser
  - Radix3 page table builder
  - WPR2 metadata configuration
  - Falcon reset and RISC-V boot
- DMA buffer allocation helpers
- RPC queue infrastructure (cmd/stat)

### Changed
- Separated GSP logic from main driver

## [0.1.0] - 2026-01-27

### Added
- Initial driver structure with IOKit
- PCI device detection for RTX 40 series GPUs
- BAR0/BAR1 memory mapping (MMIO + VRAM)
- Chip identification (architecture, implementation)
- GSP/RISC-V status register monitoring
- Support for multiple Ada Lovelace devices:
  - RTX 4090 (0x2684)
  - RTX 4090 D (0x2685)
  - RTX 4080 Super (0x2702)
  - RTX 4080 (0x2704)
  - RTX 4070 Ti Super (0x2705)
  - RTX 4070 Ti (0x2782)
  - RTX 4070 Super (0x2860)
  - RTX 4070 (0x2786)
- Register definitions header (NVDAALRegs.h)
- GSP controller structure (NVDAALGsp.h/cpp)
- VBIOS extraction tool
- Comprehensive documentation:
  - Architecture overview
  - GSP initialization guide
  - Development roadmap
- Makefile with development commands

### Technical Details
- Based on TinyGPU/tinygrad analysis
- IOKit-based kernel extension
- Compute-only focus (no display support)

[0.4.0]: https://github.com/gabrielmaialva33/NVDAAL-Driver/compare/v0.3.1...v0.4.0
[0.3.1]: https://github.com/gabrielmaialva33/NVDAAL-Driver/compare/v0.3.0...v0.3.1
[0.3.0]: https://github.com/gabrielmaialva33/NVDAAL-Driver/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/gabrielmaialva33/NVDAAL-Driver/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/gabrielmaialva33/NVDAAL-Driver/releases/tag/v0.1.0
