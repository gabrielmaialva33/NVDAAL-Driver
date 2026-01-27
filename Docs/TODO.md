# TODO - NVDAAL Compute Driver

## Phase 1: Base Structure (COMPLETE)

- [x] Create project structure
- [x] Implement basic kext with PCI detection
- [x] Map BARs (MMIO + VRAM)
- [x] Read identification registers (BOOT0, BOOT42)
- [x] Document compute-only architecture
- [x] Create register headers (NVDAALRegs.h)
- [x] Create GSP structure (NVDAALGsp.h/cpp)

## Phase 2: GSP Initialization (COMPLETE)

- [x] Download GSP firmware (gsp-570.144.bin)
- [x] Implement ELF parser for firmware
- [x] Implement DMA allocation (IOBufferMemoryDescriptor)
- [x] Build radix3 page table
- [x] Configure WPR metadata
- [x] Implement boot sequence:
  - [x] Reset FALCON
  - [x] Execute FWSEC (from VBIOS)
  - [x] Start RISC-V core
  - [x] Wait for GSP_INIT_DONE
- [x] Implement basic RPC:
  - [x] Enqueue command
  - [x] Dequeue status
  - [x] sendRpc()
  - [x] waitRpcResponse()

## Phase 3: User-Space Interface (COMPLETE)

- [x] Implement IOUserClient (NVDAALUserClient)
- [x] Secure firmware upload method
- [x] Create CLI tool (nvdaal-cli) for firmware loading
- [x] libNVDAAL.dylib (SDK Wrapper)

## Phase 4: Memory Management (COMPLETE)

- [x] Implement VRAM allocator (NVDAALMemory)
- [x] DMA buffer support (IOMemoryDescriptor)
- [x] Memory mapping for user-space (BAR1 Aperture)
- [x] Page table management (Virtual Memory / VASpace)
- [x] GSP Object Management (rmAlloc/rmFree)

## Phase 5: Compute Queues (BETA)

- [x] Create GPFIFO channel (ADA_CHANNEL_GPFIFO_A)
- [x] Implement ring buffer
- [x] Push buffer management
- [x] Command submission (User Doorbell)
- [ ] Compute class instantiation (ADA_COMPUTE_A) - In progress
- [ ] Sync primitives (Semaphores)

## Phase 6: Framework Integration

- [ ] tinygrad integration
- [ ] PyTorch backend (optional)
- [ ] API documentation
- [ ] Usage examples

## Implementation Notes

### Priorities
1. GSP is **mandatory** - nothing works on Ada Lovelace without it
2. Focus on compute queues, not display
3. Simplicity > features

### Useful Resources
- TinyGPU: `tinygrad/runtime/support/nv/ip.py`
- NVIDIA open-gpu-kernel-modules
- Nouveau project

### macOS Differences
- IOBufferMemoryDescriptor instead of dma_alloc_coherent
- IOUserClient instead of /dev/nvidia*
- No direct mmap - use IOMemoryMap

## Useful Commands

```bash
# Build
make clean && make

# Test structure
make test

# Load temporarily
make load

# View logs
make logs

# View logs in real-time
make logs-live

# Download firmware
make download-firmware
```
