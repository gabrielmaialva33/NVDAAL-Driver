/*
 * SSDT ROG Maximus Z790 Hero
 *
 * Sistema:
 *   Board:    ASUS ROG Maximus Z790 Hero
 *   CPU:      Intel Core i9-13900K (Raptor Lake-S, 24C/32T)
 *   Chipset:  Intel Z790 (Host: 8086:A700, LPC: 8086:7A04)
 *   SMBIOS:   MacPro7,1
 *
 * GPUs:
 *   Display:  AMD RX 570 (Polaris) - PEG1 - Nativo
 *   Compute:  NVIDIA RTX 4090 (AD102) - PEG2 - NVDAAL
 *
 * Base:      MaLd0n/Olarila
 * Enhance:   NVDAAL Compute + cleaned
 * Version:   4.2.0
 *
 * Data Source: Windows nvidia-smi + PCI registry (2026-02-06)
 * Updated:    2026-03-04 - Fixed IOKit UUID property injection for driver
 */
DefinitionBlock ("", "SSDT", 2, "NVDAAL", "Z790Hero", 0x00040200)
{
    // ========================================================================
    // External References
    // ========================================================================
    External (_SB_.PC00, DeviceObj)
    External (_SB_.PC00.LPCB, DeviceObj)
    External (_SB_.PC00.PEG1, DeviceObj)           // RX 570 (Display)
    External (_SB_.PC00.PEG1.PEGP, DeviceObj)
    External (_SB_.PC00.PEG2, DeviceObj)           // RTX 4090 (Compute)
    External (_SB_.PC00.PEG2.PEGP, DeviceObj)
    External (_SB_.PC00.SBUS, DeviceObj)
    External (_SB_.PC00.XHCI, DeviceObj)
    External (_SB_.PC00.XHCI._PRW, IntObj)
    External (_SB_.PC00.XHCI.RHUB, DeviceObj)
    External (_SB_.PR00, ProcessorObj)
    External (HPTE, UnknownObj)
    External (STAS, UnknownObj)

    // ========================================================================
    // Global Fixes (HPET + ACPI OS)
    // ========================================================================
    Scope (\)
    {
        If (_OSI ("Darwin"))
        {
            HPTE = Zero
            STAS = One
        }
    }

    // ========================================================================
    // CPU Power Management (XCPM) - Raptor Lake i9-13900K
    // Equivalent to SSDT-PLUG-ALT (Alder Lake+ uses _SB.PR00)
    // ========================================================================
    Scope (\_SB.PR00)
    {
        If (_OSI ("Darwin"))
        {
            Method (_DSM, 4, NotSerialized)
            {
                If (!Arg2) { Return (Buffer (One) { 0x03 }) }
                Return (Package ()
                {
                    "plugin-type", One
                })
            }
        }
    }

    // ========================================================================
    // Fake EC (Embedded Controller) - Required for macOS
    // ========================================================================
    Scope (\_SB)
    {
        Device (EC)
        {
            Name (_HID, "ACID0001")
            Method (_STA, 0, NotSerialized)
            {
                If (_OSI ("Darwin")) { Return (0x0F) }
                Return (Zero)
            }
        }
    }

    // ========================================================================
    // AWAC/RTC Fix - Required for 400+ series chipsets
    // ========================================================================
    Scope (\_SB)
    {
        Device (ARTC)
        {
            Name (_HID, "ACPI000E")
            Method (_STA, 0, NotSerialized)
            {
                If (_OSI ("Darwin")) { Return (0x0F) }
                Return (Zero)
            }
        }
    }

    // ========================================================================
    // USB Power Properties - MacPro7,1 high power
    // ========================================================================
    Scope (\_SB)
    {
        Device (USBX)
        {
            Name (_ADR, Zero)
            Method (_DSM, 4, NotSerialized)
            {
                If (!Arg2) { Return (Buffer (One) { 0x03 }) }
                Return (Package ()
                {
                    "kUSBSleepPowerSupply",     0x13EC,
                    "kUSBSleepPortCurrentLimit", 0x0834,
                    "kUSBWakePowerSupply",      0x13EC,
                    "kUSBWakePortCurrentLimit", 0x0834
                })
            }
            Method (_STA, 0, NotSerialized)
            {
                If (_OSI ("Darwin")) { Return (0x0F) }
                Return (Zero)
            }
        }
    }

    // ========================================================================
    // USB Wake Device
    // ========================================================================
    If ((CondRefOf (\_OSI, Local0) && _OSI ("Darwin")))
    {
        Device (\_SB.USBW)
        {
            Name (_HID, "PNP0D10")
            Name (_UID, "WAKE")
            Method (_PRW, 0, NotSerialized)
            {
                Return (\_SB.PC00.XHCI._PRW)
            }
        }
    }

    // ========================================================================
    // LPC Bridge Devices
    // ========================================================================
    Scope (\_SB.PC00.LPCB)
    {
        Device (PMCR)
        {
            Name (_HID, EisaId ("APP9876"))
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0xFE000000, 0x00010000)
            })
            Method (_STA, 0, NotSerialized)
            {
                If (_OSI ("Darwin")) { Return (0x0B) }
                Return (Zero)
            }
        }

        Device (FWHD)
        {
            Name (_HID, EisaId ("INT0800"))
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadOnly, 0xFF000000, 0x01000000)
            })
            Method (_STA, 0, NotSerialized)
            {
                If (_OSI ("Darwin")) { Return (0x0B) }
                Return (Zero)
            }
        }

        Device (DMAC)
        {
            Name (_HID, EisaId ("PNP0200"))
            Name (_CRS, ResourceTemplate ()
            {
                IO (Decode16, 0x0000, 0x0000, 0x01, 0x20)
                IO (Decode16, 0x0081, 0x0081, 0x01, 0x11)
                IO (Decode16, 0x0093, 0x0093, 0x01, 0x0D)
                IO (Decode16, 0x00C0, 0x00C0, 0x01, 0x20)
                DMA (Compatibility, NotBusMaster, Transfer8_16) {4}
            })
            Method (_STA, 0, NotSerialized)
            {
                If (_OSI ("Darwin")) { Return (0x0B) }
                Return (Zero)
            }
        }
    }

    // ========================================================================
    // SMBus Devices
    // ========================================================================
    Scope (\_SB.PC00.SBUS)
    {
        Device (BUS0)
        {
            Name (_CID, "smbus")
            Name (_ADR, Zero)
            Method (_STA, 0, NotSerialized)
            {
                If (_OSI ("Darwin")) { Return (0x0F) }
                Return (Zero)
            }
        }

        Device (BUS1)
        {
            Name (_CID, "smbus")
            Name (_ADR, One)
            Method (_STA, 0, NotSerialized)
            {
                If (_OSI ("Darwin")) { Return (0x0F) }
                Return (Zero)
            }
        }
    }

    // ========================================================================
    // PEG1 - AMD RX 570 (Display GPU) - Nativo
    // Path: PciRoot(0x0)/Pci(0x1,0x0)/Pci(0x0,0x0)
    // ========================================================================
    Scope (\_SB.PC00.PEG1.PEGP)
    {
        Method (_DSM, 4, NotSerialized)
        {
            If ((_OSI ("Darwin") && LEqual (Arg0, ToUUID ("a0b5b7c6-1318-441c-b0c9-fe695eaf949b"))))
            {
                If (!Arg2) { Return (Buffer (One) { 0x03 }) }
                Return (Package ()
                {
                    "model",                Buffer () { "AMD Radeon RX 570" },
                    "AAPL,slot-name",       Buffer () { "Slot-1" },
                    "built-in",             Buffer () { 0x00 },
                    "@0,AAPL,boot-display", Buffer () { 0x01, 0x00, 0x00, 0x00 }
                })
            }
            Return (Buffer (One) { 0x00 })
        }
    }

    // ========================================================================
    // PEG2 - NVIDIA RTX 4090 (Compute GPU) - NVDAAL
    // Path: PciRoot(0x0)/Pci(0x1,0x1)/Pci(0x0,0x0)
    //
    // Hardware (confirmed via nvidia-smi + Windows registry 2026-02-06):
    //   GPU:      AD102-300-A1, arch 0x190, impl 0x02, rev 0xA1
    //   Part:     2684-300-A1
    //   DevID:    0x2684 (NVIDIA), SubSys: 0x889D1043 (ASUS ROG STRIX)
    //   VBIOS:    95.02.18.80.87, Inforom: G002.0000.00.03
    //   VRAM:     24564 MiB GDDR6X, 384-bit, 10501 MHz (422 MiB reserved)
    //   BAR1:     256 MiB
    //   CUDA:     Compute 8.9, 16384 cores, max SM 3105 MHz
    //   PCIe:     Gen4 x16, Bus 02:00.0 (behind PEG2 bridge 8086:A72D)
    //   Power:    450W TDP (BIOS OC: 477W), 150W min, 600W max
    //   Thermal:  84C target, 99C slowdown, 104C shutdown
    //   Display:  Disabled, No display attached
    //   UUID:     GPU-4f1ceff0-0dc0-9286-8453-1bb6657cb77a
    // ========================================================================
    // PEG2 bridge _DSM REMOVED - may conflict with DSDT OEM _DSM
    // Slot-name moved to PEG2.PEGP IOKit _DSM instead

    // PEG2.PEGP - RTX 4090 - ONLY _DSM, nothing else
    // REMOVED: _DDN (Name conflict), _STA (duplicate), _S0W (D3cold kills HDMI Audio)
    // IOPCIBridge manages power/presence — we only provide data via _DSM
    Scope (\_SB.PC00.PEG2.PEGP)
    {
        Method (_DSM, 4, NotSerialized)
        {
            // NVDAAL Private _DSM (all OSes - driver reads via ACPI)
            If (LEqual (Arg0, ToUUID ("4e564441-414c-0000-0000-000000000000")))
            {
                If (LEqual (Arg2, Zero)) { Return (Buffer (One) { 0x7F }) }

                If (LEqual (Arg2, One))
                {
                    Return (Package ()
                    {
                        "device-type",        "compute",
                        "gpu-architecture",   "ada-lovelace",
                        "gpu-chip",           "AD102",
                        "gpu-part-number",    "2684-300-A1",
                        "arch-id",            0x0190,
                        "impl-id",            0x02,
                        "revision-id",        0xA1,
                        "pci-device-id",      0x2684,
                        "pci-subsystem-id",   0x889D1043,
                        "cuda-compute",       Buffer () { 0x08, 0x09 },
                        "cuda-cores",         0x4000,
                        "gpu-max-clock-mhz",  0x0C21,
                        "vbios-version",      "95.02.18.80.87",
                        "inforom-version",    "G002.0000.00.03"
                    })
                }

                If (LEqual (Arg2, 0x02))
                {
                    Return (Package ()
                    {
                        "vram-total",     Buffer () { 0x00, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00 },
                        "vram-usable",    Buffer () { 0x00, 0x00, 0x40, 0xFF, 0x05, 0x00, 0x00, 0x00 },
                        "vram-type",      "GDDR6X",
                        "vram-bus-width", 0x0180,
                        "vram-clock-mhz", 0x2905,
                        "bar1-size",      0x10000000,
                        "wpr2-base",      Buffer () { 0x00, 0x00, 0x40, 0xFF, 0x05, 0x00, 0x00, 0x00 },
                        "wpr2-size",      0x00C00000
                    })
                }

                If (LEqual (Arg2, 0x03))
                {
                    Return (Package ()
                    {
                        "tdp-watts",            0x01C2,
                        "power-limit-bios-w",   0x01DD,
                        "power-min-watts",      0x0096,
                        "power-max-watts",      0x0258,
                        "thermal-target-c",     0x54,
                        "thermal-slowdown-c",   0x63,
                        "thermal-shutdown-c",   0x68
                    })
                }

                If (LEqual (Arg2, 0x04))
                {
                    Return (Package ()
                    {
                        "gsp-falcon-base",     0x00110000,
                        "sec2-falcon-base",    0x00840000,
                        "gsp-riscv-base",      0x00118000,
                        "fwsec-source",        "vbios",
                        "fwsec-target",        "gsp-falcon",
                        "wpr2-addr-lo-reg",    0x001FA824,
                        "wpr2-addr-hi-reg",    0x001FA828,
                        "frts-status-reg",     0x00001438,
                        "fwsec-sb-status-reg", 0x00001454,
                        "gsp-fw-version",      "570.144",
                        "fuse-ver-reg-base",   0x008241C0
                    })
                }

                If (LEqual (Arg2, 0x05))
                {
                    Return (Package ()
                    {
                        "nvdaal-boot-mode",   "linux-compat",
                        "gsp-warm-boot",      One,
                        "skip-display-init",  One,
                        "fwsec-already-run",  One,
                        "prefer-pio-load",    One,
                        "debug-level",        Zero
                    })
                }

                If (LEqual (Arg2, 0x06))
                {
                    Return (Package ()
                    {
                        "pcie-max-speed",       0x04,
                        "pcie-max-width",       0x10,
                        "pcie-slot-gen",        0x05,
                        "pcie-bus-number",      0x02,
                        "parent-bridge-dev-id", 0xA72D,
                        "platform-vendor",      "ASUS",
                        "platform-board",       "ROG Maximus Z790 Hero",
                        "gpu-board-vendor",     "ASUS",
                        "gpu-board-model",      "ROG STRIX RTX 4090",
                        "gpu-board-id",         0x0200,
                        "gpu-uuid",             "GPU-4f1ceff0-0dc0-9286-8453-1bb6657cb77a"
                    })
                }
            }

            // macOS IOKit properties (Darwin only)
            // CRITICAL: The driver reads properties via pciDevice->getProperty()
            // which only sees properties injected through this Apple UUID.
            // All NVDAAL boot-time properties MUST be here, not in the private UUID.
            If ((_OSI ("Darwin") && LEqual (Arg0, ToUUID ("a0b5b7c6-1318-441c-b0c9-fe695eaf949b"))))
            {
                If (!Arg2) { Return (Buffer (One) { 0x03 }) }
                Return (Package ()
                {
                    // Standard IOKit properties
                    "model",             Buffer () { "NVIDIA GeForce RTX 4090" },
                    "device_type",       Buffer () { "compute" },
                    "AAPL,slot-name",    Buffer () { "Slot-2" },
                    "built-in",          Buffer () { 0x00 },
                    "nvdaal-compatible", One,
                    "nvdaal-version",    0x0007,
                    "pci-aspm-default",  Zero,

                    // NVDAAL boot hints (Function 5 - driver reads via getProperty)
                    // nvdaal-boot-mode: driver reads as OSData, compare bytes
                    "nvdaal-boot-mode",  Buffer () { "linux-compat" },
                    "gsp-warm-boot",     One,
                    "skip-display-init", One,
                    "fwsec-already-run", One,
                    "prefer-pio-load",   One,
                    "debug-level",       Zero,

                    // Hardware parameters (driver validates against detected)
                    "arch-id",           0x0190,
                    "gsp-falcon-base",   0x00110000,
                    "sec2-falcon-base",  0x00840000,
                    "vram-usable",       Buffer () { 0x00, 0x00, 0x40, 0xFF, 0x05, 0x00, 0x00, 0x00 }
                })
            }

            Return (Buffer (One) { 0x00 })
        }
    }

    // ========================================================================
    // XHCI Root Hub - Disable RHUB for USBMap.kext
    // ========================================================================
    Scope (\_SB.PC00.XHCI.RHUB)
    {
        Method (_STA, 0, NotSerialized)
        {
            If (_OSI ("Darwin")) { Return (Zero) }
            Return (0x0F)
        }
    }
}
