# ACTION ITEMS - NVDAAL Driver Fixes

> Based on reverse engineering session 2026-02-06
> RTX 4090 (AD102) with NVIDIA driver 591.74 loaded
> Updated with findings from nouveau, open-gpu-kernel-modules, and nova-core source analysis

## Bug #1: WPR2 Register Offsets - FIXED

**Status**: FIXED (commit a203951)

Ada Lovelace WPR2 registers:
```c
#define NV_PFB_PRI_MMU_WPR2_ADDR_LO  0x001FA824
#define NV_PFB_PRI_MMU_WPR2_ADDR_HI  0x001FA828
```
Evidence: nvlddmkm.sys 591.74, nouveau tu102.c, nova-core docs.

## Bug #2: FWSEC Execution Target - FIXED (CORRECTED TWICE)

**Status**: FIXED (this commit)

### History of this bug:
1. Original code: `NV_PGSP_BASE` (0x110000) - **was actually correct**
2. Previous fix (commit 3b37dc4): changed to `NV_PSEC_BASE` (0x840000) - **WRONG**
3. This fix: reverted back to `NV_PGSP_BASE` (0x110000) - **CORRECT**

### The truth about FWSEC vs Booter targets:
| Firmware | Falcon | Base | Source |
|----------|--------|------|--------|
| **FWSEC** (FRTS/SB) | **GSP Falcon** | 0x110000 | VBIOS ROM (AppID 0x85) |
| Booter load | SEC2 Falcon | 0x840000 | linux-firmware |
| Booter unload | SEC2 Falcon | 0x840000 | linux-firmware |
| Scrubber | SEC2 Falcon | 0x840000 | linux-firmware |
| GSP-RM | GSP RISC-V | 0x110000/0x118000 | linux-firmware |

### Evidence:
- nouveau `fwsec.c`: `nvkm_falcon_fw_boot(&fw, &gsp->falcon)` (GSP falcon)
- open-gpu-kernel-modules: `kgspExecuteHsFalcon_HAL` targets GSP
- nova-core docs: "FWSEC runs on GSP Falcon in Heavy-Secure mode"
- `RMExecuteFwsecOnSec2` in nvlddmkm.sys refers to the **command** being
  "execute FWSEC [which configures] On-SEC2 [boot]", not the target falcon

## Bug #3: VBIOS FWSEC Parsing - FIXED (CORRECTED)

**Status**: FIXED (this commit)

### Previous (WRONG) assumption:
"Ada VBIOS does NOT contain FWSEC" - **INCORRECT**

### The truth:
Ada VBIOS DOES contain FWSEC. It's just not a PCI ROM image (codeType 0xE0).
FWSEC is stored in the BIT (BIOS Information Table) Falcon Ucode Table:

```
VBIOS ROM
  └─ BIT Header (signature 0x00544942 = "BIT\0")
       └─ Token 0x70 (Falcon Data)
            └─ Falcon Ucode Table
                 └─ Entry with AppID 0x85 = FWSEC production
                      └─ Descriptor V3 (44 bytes) + RSA-3K signatures
                           └─ IMEM + DMEM (the actual FWSEC microcode)
```

The linux-firmware package does NOT contain separate fwsec files.
Every NVIDIA driver (proprietary, nouveau, nova-core) extracts FWSEC from VBIOS at runtime.

## Complete Ada Lovelace Boot Sequence

```
[1] GPU Power-On / Reset

[2] Driver reads VBIOS from GPU ROM (PROM at 0x300000)

[3] FWSEC-FRTS extracted from VBIOS (BIT Token 0x70, AppID 0x85, desc V3)
    - Target: GSP Falcon (0x110000) in Heavy-Secure mode
    - Patches DMEM with FRTS command (0x15)
    - Selects RSA-3K signature via fuse version (reg 0x8241C0 + (ucodeId-1)*4)
    - Executes: carves out WPR2 (1MB FRTS region at top of VRAM)
    - Verify: FRTS status = reg 0x001438 (upper 16 bits = 0 = success)
    - Verify: WPR2 at 0x1FA824 (LO) / 0x1FA828 (HI)

[4] Booter Load firmware (booter_load-570.144.bin)
    - Target: SEC2 Falcon (0x840000) in Heavy-Secure mode
    - Loads and verifies GSP-RM into WPR2

[5] GSP-RM boot (gsp-570.144.bin via bootloader-570.144.bin)
    - Target: GSP Falcon in RISC-V mode
    - Takes over GPU resource management
```

## Data: Live GPU State (driver loaded)

| Property | Value |
|----------|-------|
| Driver | 591.74 |
| NVML | 13.590.52.01 |
| CUDA | 13.1 (13010) |
| VRAM Usable | 0x5FF400000 (23.99 GB) |
| VRAM Physical | 0x600000000 (24.0 GB) |
| FW Reserved | ~12 MB (WPR2 region) |
| BAR1 | 256 MB |
| VBIOS | 95.02.18.80.87 |
| PCIe | Gen4 x8 (max x16) |
| Temp | 34C idle |
| Power | 15W idle / 477W max |

## WPR2 Expected Values (live confirmed)

```
Physical VRAM:    0x600000000 (24 GB)
Usable VRAM:      0x5FF400000 (23.99 GB)
WPR2 reserved:    12 MB (top of VRAM)

WPR2_ADDR_LO (0x1FA824): 0x5FF400  (0x5FF400000 >> 12)
WPR2_ADDR_HI (0x1FA828): 0x600000  (0x600000000 >> 12)
```

## Key Registers

| Register | Address | Purpose |
|----------|---------|---------|
| WPR2_ADDR_LO | 0x1FA824 | WPR2 lower bound (page-shifted) |
| WPR2_ADDR_HI | 0x1FA828 | WPR2 upper bound (bit 31 = enabled) |
| FRTS status | 0x001438 | Upper 16 bits = FRTS error (0 = ok) |
| FWSEC-SB status | 0x001454 | Lower 16 bits = SB error (0 = ok) |
| Fuse version | 0x8241C0 | + (ucodeId-1)*4 per ucode |
| PROM base | 0x300000 | VBIOS ROM access via BAR0 |

## Firmware Files Status

| File | Target | Size | Status |
|------|--------|------|--------|
| booter_load-570.144.bin | SEC2 | 57,720 | OK |
| booter_unload-570.144.bin | SEC2 | 41,592 | OK |
| bootloader-570.144.bin | GSP | 36,972 | OK |
| scrubber-570.144.bin | SEC2 | 8,312 | OK |
| fwsignature_ad10x.bin | - | 4,096 | OK |
| gsp-570.144.bin | GSP RISC-V | ~70 MB | symlink to ga102 |
| **FWSEC** | **GSP Falcon** | **in VBIOS** | **Extracted at runtime** |

## Sources

- [nouveau fwsec.c](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/nouveau/nvkm/subdev/gsp/fwsec.c)
- [open-gpu-kernel-modules kernel_gsp_fwsec.c](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/main/src/nvidia/src/kernel/gpu/gsp/kernel_gsp_fwsec.c)
- [open-gpu-kernel-modules kernel_gsp_frts_tu102.c](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/main/src/nvidia/src/kernel/gpu/gsp/arch/turing/kernel_gsp_frts_tu102.c)
- [nova-core FWSEC docs](https://docs.kernel.org/gpu/nova/core/fwsec.html)
- [NVIDIA Falcon Security](https://nvidia.github.io/open-gpu-doc/Falcon-Security/Falcon-Security.html)
