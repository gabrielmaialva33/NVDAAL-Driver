# NVDAAL FWSEC Executor

EFI driver that executes NVIDIA FWSEC-FRTS to configure WPR2 before macOS boots.

## Purpose

On Hackintosh systems, the NVIDIA VBIOS POST doesn't fully execute, leaving WPR2 (Write Protected Region 2) unconfigured. This prevents the GSP (GPU System Processor) from booting properly.

This driver:
1. Finds the NVIDIA GPU during EFI boot
2. Reads and parses the VBIOS from GPU BAR
3. Extracts FWSEC microcode from the PMU Lookup Table
4. Patches DMEMMAPPER to execute FRTS command
5. Loads and executes FWSEC on GSP Falcon
6. Configures WPR2 for GSP boot

## Building

### Prerequisites

- [EDK2](https://github.com/tianocore/edk2) build environment
- GCC or Clang toolchain

### Build Steps

```bash
# 1. Set up EDK2
cd /path/to/edk2
source edksetup.sh
export PACKAGES_PATH=$WORKSPACE

# 2. Run build script
cd /path/to/NVDAAL-Driver/EFI/NvdaalFwsec
chmod +x build.sh
./build.sh
```

### Manual Build

```bash
# Copy to EDK2 workspace
cp -r NvdaalFwsec $WORKSPACE/NvdaalPkg

# Build
build -p NvdaalPkg/NvdaalPkg.dsc -a X64 -t GCC5 -b RELEASE
```

## Installation

1. Copy `NvdaalFwsec.efi` to `EFI/OC/Drivers/` on your EFI partition

2. Add to `config.plist` under `UEFI -> Drivers`:
```xml
<dict>
    <key>Arguments</key>
    <string></string>
    <key>Comment</key>
    <string>NVIDIA FWSEC for WPR2</string>
    <key>Enabled</key>
    <true/>
    <key>LoadEarly</key>
    <true/>
    <key>Path</key>
    <string>NvdaalFwsec.efi</string>
</dict>
```

3. Reboot and check OpenCore boot log for NVDAAL messages

## Supported GPUs

- NVIDIA RTX 4090 (0x2684, 0x2685)
- NVIDIA RTX 4080 (0x2702)
- NVIDIA RTX 4070 Ti (0x2782)

## How It Works

```
EFI Boot
    │
    ▼
┌──────────────────────────────┐
│  NvdaalFwsec.efi loads       │
│  (before other GPU drivers)  │
└──────────────────────────────┘
    │
    ▼
┌──────────────────────────────┐
│  Find NVIDIA GPU via PCI IO  │
│  Map BAR0 (MMIO registers)   │
└──────────────────────────────┘
    │
    ▼
┌──────────────────────────────┐
│  Read VBIOS from BAR0+0x300000│
│  Parse BIT table             │
│  Find PMU Lookup Table       │
│  Extract FWSEC ucode         │
└──────────────────────────────┘
    │
    ▼
┌──────────────────────────────┐
│  Patch DMEMMAPPER initCmd    │
│  Set to 0x15 (FRTS command)  │
└──────────────────────────────┘
    │
    ▼
┌──────────────────────────────┐
│  Load IMEM/DMEM to Falcon    │
│  Set boot vector             │
│  Start Falcon execution      │
└──────────────────────────────┘
    │
    ▼
┌──────────────────────────────┐
│  FWSEC executes              │
│  Configures WPR2 region      │
└──────────────────────────────┘
    │
    ▼
┌──────────────────────────────┐
│  macOS boots                 │
│  NVDAAL.kext can now         │
│  boot GSP successfully       │
└──────────────────────────────┘
```

## Troubleshooting

### "No compatible NVIDIA GPU found"
- Ensure GPU is properly seated in PCIe slot
- Check if GPU is recognized in UEFI firmware

### "BIT header not found"
- VBIOS may be corrupted or have non-standard format
- Try with a different VBIOS version

### "FWSEC completed but WPR2 not configured"
- Hardware signature verification may have failed
- Check if VBIOS is original/unmodified
- May need alternative approach (CSM boot)

### Check boot log
In OpenCore, enable debug logging:
```xml
<key>Misc</key>
<dict>
    <key>Debug</key>
    <dict>
        <key>Target</key>
        <integer>67</integer>
    </dict>
</dict>
```

## References

- [Linux nova-core FWSEC](https://docs.kernel.org/gpu/nova/core/fwsec.html)
- [NVIDIA BIOS Information Table](https://download.nvidia.com/open-gpu-doc/BIOS-Information-Table/1/BIOS-Information-Table.html)
- [Falcon Microcontroller](https://docs.kernel.org/gpu/nova/core/falcon.html)

## License

MIT License - See LICENSE file
