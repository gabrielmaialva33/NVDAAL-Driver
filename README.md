<h1 align="center">
  <br>
  <img src=".github/assets/logo.svg" alt="NVDAAL" width="200">
  <br>
  NVDAAL - NVIDIA Ada Lovelace Compute Driver
  <br>
</h1>

<p align="center">
  <strong>Open-source compute driver for RTX 40 series GPUs on macOS - Pure AI/ML power</strong>
</p>

<p align="center">
  <img src="https://img.shields.io/github/license/gabrielmaialva33/NVDAAL-Driver?color=00b8d3&style=flat&logo=appveyor" alt="License" />
  <img src="https://img.shields.io/badge/C++-IOKit-00599C?style=flat&logo=cplusplus" alt="Language" />
  <img src="https://img.shields.io/badge/macOS-Tahoe%2026+-000?style=flat&logo=apple" alt="Platform" />
  <img src="https://img.shields.io/badge/NVIDIA-RTX%2040%20Series-76B900?style=flat&logo=nvidia" alt="GPU" />
  <img src="https://img.shields.io/github/repo-size/gabrielmaialva33/NVDAAL-Driver?style=flat&logo=appveyor" alt="Repo Size" />
  <a href="https://github.com/gabrielmaialva33/NVDAAL-Driver/commits/master">
    <img src="https://img.shields.io/github/last-commit/gabrielmaialva33/NVDAAL-Driver?style=flat&logo=appveyor" alt="Last Commit" />
  </a>
  <img src="https://img.shields.io/badge/made%20by-Maia-15c3d6?style=flat&logo=appveyor" alt="Made by Maia" />
</p>

<br>

<p align="center">
  <a href="#bookmark-about">About</a>&nbsp;&nbsp;&nbsp;|&nbsp;&nbsp;&nbsp;
  <a href="#rocket-quick-start">Quick Start</a>&nbsp;&nbsp;&nbsp;|&nbsp;&nbsp;&nbsp;
  <a href="#wrench-features">Features</a>&nbsp;&nbsp;&nbsp;|&nbsp;&nbsp;&nbsp;
  <a href="#gear-architecture">Architecture</a>&nbsp;&nbsp;&nbsp;|&nbsp;&nbsp;&nbsp;
  <a href="#memo-roadmap">Roadmap</a>&nbsp;&nbsp;&nbsp;|&nbsp;&nbsp;&nbsp;
  <a href="#handshake-contributing">Contributing</a>&nbsp;&nbsp;&nbsp;|&nbsp;&nbsp;&nbsp;
  <a href="#scroll-license">License</a>
</p>

<br>

## :bookmark: About

**NVDAAL** (NVIDIA Ada Lovelace) is an open-source **compute-only** driver for NVIDIA RTX 40 series GPUs on macOS Hackintosh. This driver focuses exclusively on **AI/ML workloads**, leveraging the full compute power of Ada Lovelace architecture without display overhead.

### :warning: Important Notice

This project is **experimental** and in early development. It requires:
- Deep understanding of GPU architecture
- Hackintosh environment with proper configuration
- GSP (GPU System Processor) firmware for full functionality

### :zap: Why Compute-Only?

| Aspect | Benefit |
|--------|---------|
| **Simplicity** | No framebuffer, display engine, or video output code |
| **Focus** | 100% of GPU power dedicated to compute workloads |
| **Viability** | Based on proven TinyGPU implementation |
| **Performance** | Direct access to CUDA cores and Tensor cores |

## :desktop_computer: Supported Hardware

| GPU | Device ID | CUDA Cores | Tensor Cores | Status |
|-----|-----------|------------|--------------|--------|
| RTX 4090 | `0x2684` | 16,384 | 512 | :construction: Development |
| RTX 4090 D | `0x2685` | 14,592 | 456 | :hourglass: Planned |
| RTX 4080 Super | `0x2702` | 10,240 | 320 | :hourglass: Planned |
| RTX 4080 | `0x2704` | 9,728 | 304 | :hourglass: Planned |
| RTX 4070 Ti Super | `0x2705` | 8,448 | 264 | :hourglass: Planned |
| RTX 4070 Ti | `0x2782` | 7,680 | 240 | :hourglass: Planned |
| RTX 4070 Super | `0x2860` | 7,168 | 224 | :hourglass: Planned |
| RTX 4070 | `0x2786` | 5,888 | 184 | :hourglass: Planned |

## :rocket: Quick Start

### :heavy_check_mark: Prerequisites

- macOS Tahoe 26+ (via OpenCore 1.0.7+)
- Xcode Command Line Tools
- NVIDIA RTX 40 series GPU
- Boot args: `kext-dev-mode=1` or `amfi_get_out_of_my_way=0x1`

### :arrow_down: Installation

```bash
# Clone the repository
git clone https://github.com/gabrielmaialva33/NVDAAL-Driver.git
cd NVDAAL-Driver

# Download GSP firmware
make download-firmware

# Build the kext + tools + library
make clean && make

# Validate structure
make test

# Load temporarily (for testing)
make load

# Check logs
make logs
```

### :package: Permanent Installation

```bash
# Install to /Library/Extensions
make install

# Reboot required
sudo reboot
```

## :wrench: Features

### Current (v0.3.0 - Pioneer)
- :white_check_mark: PCI device detection and enumeration
- :white_check_mark: BAR0/BAR1 memory mapping (MMIO + VRAM)
- :white_check_mark: Chip identification (Ada Lovelace architecture)
- :white_check_mark: **GSP Controller Implementation**
  - ELF Firmware Parser
  - Radix3 Page Table Builder
  - WPR2 Metadata Configuration
  - Secure Boot Sequence (Falcon Reset -> RISC-V Boot)
  - **Full RPC Engine** (rmAlloc, rmControl)
- :white_check_mark: **Memory Management (MMU)**
  - Virtual Address Space (VASpace)
  - Page Directory/Table management
- :white_check_mark: **Compute Engine**
  - GPFIFO Channel creation
  - User Doorbell mapping
  - Command Submission
- :white_check_mark: **User-Space Interface**
  - IOUserClient for secure firmware upload
  - Zero-copy memory mapping
  - libNVDAAL shared library
- :white_check_mark: **CLI Tool** (nvdaal-cli)

### In Development
- :construction: Compute Class (ADA_COMPUTE_A) Context
- :construction: Semaphore Synchronization

### Planned
- :hourglass: tinygrad/PyTorch integration

## :gear: Architecture

### System Overview

```mermaid
graph TB
    subgraph "User Space"
        CLI[nvdaal-cli]
        PY[Python Scripts]
        ML[tinygrad / PyTorch]
        LIB[libNVDAAL.dylib]
    end

    subgraph "Kernel Space (NVDAAL.kext)"
        UC[NVDAALUserClient]
        DEV[NVDAAL IOService]
        MEM[NVDAALMemory]
        QUEUE[NVDAALQueue]
        GSP[NVDAALGsp]
        DISP[NVDAALDisplay]
    end

    subgraph "Hardware (RTX 4090)"
        RISCV[GSP RISC-V Core]
        SM[128 SMs / 16384 CUDA Cores]
        TENSOR[512 Tensor Cores]
        VRAM[24GB GDDR6X]
    end

    CLI --> LIB
    PY --> LIB
    ML --> LIB
    LIB --> UC
    UC --> DEV
    DEV --> MEM
    DEV --> QUEUE
    DEV --> GSP
    DEV --> DISP
    GSP -->|RPC Protocol| RISCV
    QUEUE -->|Compute Commands| SM
    SM --> TENSOR
    MEM -->|BAR1 Mapping| VRAM
```

### GSP Boot Sequence

```mermaid
sequenceDiagram
    participant User as nvdaal-cli
    participant Lib as libNVDAAL
    participant Drv as NVDAAL.kext
    participant GSP as NVDAALGsp
    participant HW as GPU Hardware

    User->>Lib: loadFirmware(gsp.bin)
    Lib->>Drv: IOUserClient call
    Drv->>GSP: Initialize GSP
    GSP->>GSP: Parse ELF firmware
    GSP->>GSP: Build Radix3 page tables
    GSP->>GSP: Configure WPR2 metadata
    GSP->>HW: Reset Falcon core
    GSP->>HW: Load bootloader ucode
    GSP->>HW: Start RISC-V execution
    HW-->>GSP: GSP_INIT_DONE event
    GSP->>GSP: Setup RPC queues
    GSP-->>Drv: Ready
    Drv-->>Lib: Success
    Lib-->>User: Firmware loaded
```

### Memory Layout

```mermaid
graph LR
    subgraph "BAR0 - MMIO (16MB)"
        PMC[PMC Registers]
        FALCON[Falcon Control]
        RISCV_CTRL[RISC-V Control]
        GSP_QUEUE[GSP Queues]
    end

    subgraph "BAR1 - VRAM (24GB)"
        USER_MEM[User Memory]
        GSP_HEAP[GSP Heap<br/>129MB]
        WPR2[WPR2 Region<br/>Protected]
        FRTS[FRTS Scratch<br/>1MB]
    end

    subgraph "System RAM"
        CMD_Q[Command Queue<br/>256KB]
        STAT_Q[Status Queue<br/>256KB]
        FW_BUF[Firmware Buffer<br/>~30MB]
    end

    PMC -.->|Control| USER_MEM
    GSP_QUEUE -.->|RPC| GSP_HEAP
```

### Component Interaction

```mermaid
graph TD
    subgraph "NVDAAL.kext Components"
        A[NVDAAL<br/>Main IOService] --> B[NVDAALGsp<br/>GSP Controller]
        A --> C[NVDAALMemory<br/>VRAM Allocator]
        A --> D[NVDAALQueue<br/>Command Queue]
        A --> E[NVDAALDisplay<br/>Fake Display]
        A --> F[NVDAALUserClient<br/>User Interface]

        B --> |"parseElfFirmware()"| B1[ELF Parser]
        B --> |"buildRadix3PageTable()"| B2[Page Tables]
        B --> |"boot()"| B3[Boot Sequence]
        B --> |"sendRpc()"| B4[RPC Protocol]

        C --> |"allocVram()"| C1[Linear Allocator]
        D --> |"push() / kick()"| D1[Ring Buffer]
    end
```

## :memo: Roadmap

| Phase | Description | Status |
|-------|-------------|--------|
| **1. Foundation** | PCI detection, BAR mapping, chip ID | :white_check_mark: Complete |
| **2. GSP Init** | Firmware loading, RPC setup, boot sequence | :white_check_mark: Complete |
| **3. User API** | libNVDAAL, IOUserClient, CLI tool | :white_check_mark: Complete |
| **4. Memory** | VRAM allocation, DMA buffers, virtual memory | :construction: In Progress |
| **5. Compute** | Queue management, command submission, sync | :hourglass: Planned |
| **6. Integration** | tinygrad, PyTorch backends | :hourglass: Planned |

## :open_file_folder: Project Structure

```
NVDAAL-Driver/
├── Sources/                  # Kernel extension source
│   ├── NVDAAL.{h,cpp}       # Main IOService driver
│   ├── NVDAALGsp.{h,cpp}    # GSP controller & RPC
│   ├── NVDAALUserClient.{h,cpp}  # User-space interface
│   ├── NVDAALMemory.{h,cpp} # VRAM allocator
│   ├── NVDAALQueue.{h,cpp}  # Command queue
│   ├── NVDAALDisplay.{h,cpp}# Fake display engine
│   └── NVDAALRegs.h         # Register definitions
├── Library/                  # User-space SDK
│   ├── libNVDAAL.{h,cpp}    # C++ API wrapper
│   └── nvdaal_c_api.cpp     # C FFI bindings
├── Tools/
│   ├── nvdaal-cli/          # CLI firmware loader
│   ├── extract_vbios.py     # VBIOS extraction
│   └── test_driver.py       # Python test harness
├── Docs/                     # Technical documentation
│   ├── ARCHITECTURE.md      # Component details
│   ├── GSP_INIT.md          # GSP boot guide
│   └── TODO.md              # Development checklist
├── Firmware/                 # User-provided firmware
├── Info.plist               # Kext configuration
├── Makefile                 # Build system
└── README.md
```

## :handshake: Contributing

Contributions are welcome! Please read our [Contributing Guide](CONTRIBUTING.md) before submitting a PR.

### Development Commands

```bash
make clean           # Clean build artifacts
make                 # Build kext + tools + library
make rebuild         # Clean + build
make test            # Validate kext structure
make load            # Load kext temporarily
make unload          # Unload kext
make logs            # View driver logs (last 5 min)
make logs-live       # Stream logs in real-time
make status          # Check kext and PCI status
make download-firmware  # Download GSP firmware
```

## :books: Resources

- [TinyGPU/tinygrad](https://github.com/tinygrad/tinygrad) - Primary reference for GSP
- [NVIDIA open-gpu-kernel-modules](https://github.com/NVIDIA/open-gpu-kernel-modules) - Official open-source drivers
- [Nouveau Project](https://nouveau.freedesktop.org/) - Linux open-source NVIDIA driver
- [envytools](https://github.com/envytools/envytools) - NVIDIA GPU documentation

## :warning: Disclaimer

This project is for **educational and research purposes only**. There is no guarantee of functionality. Use of proprietary firmware may violate NVIDIA's license terms. Use at your own risk.

## :scroll: License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

---

<p align="center">
  Made with :purple_heart: by <a href="https://github.com/gabrielmaialva33">Gabriel Maia</a>
</p>
