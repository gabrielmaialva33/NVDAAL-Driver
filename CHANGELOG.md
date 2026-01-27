# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- GSP firmware loading support (in progress)
- RPC communication infrastructure (in progress)
- DMA buffer allocation (in progress)

### Changed
- Refactored to compute-only architecture

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

[Unreleased]: https://github.com/gabrielmaialva33/NVDAAL-Driver/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/gabrielmaialva33/NVDAAL-Driver/releases/tag/v0.1.0
