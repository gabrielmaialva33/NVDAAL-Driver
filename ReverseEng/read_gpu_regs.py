#!/usr/bin/env python3
"""
read_gpu_regs.py - Read key GPU registers from NVIDIA RTX 4090 (AD102) via BAR0 MMIO on Linux.

Opens /sys/bus/pci/devices/0000:02:00.0/resource0 and reads critical registers
using mmap for direct MMIO access.

Must be run as root (sudo).
"""

import os
import sys
import mmap
import struct
import time

# ============================================================================
# Configuration
# ============================================================================

PCI_DEVICE = "0000:02:00.0"
RESOURCE0_PATH = f"/sys/bus/pci/devices/{PCI_DEVICE}/resource0"
ROM_PATH = f"/sys/bus/pci/devices/{PCI_DEVICE}/rom"
BAR0_SIZE = 16 * 1024 * 1024  # 16MB

# PROM space for VBIOS reading
NV_PROM_BASE = 0x00180000
VBIOS_MAX_SIZE = 0x100000  # 1MB

# ============================================================================
# Register Definitions
# ============================================================================

REGISTERS = {
    # --- Chip Identification ---
    "chip_id": [
        (0x00000000, "NV_PMC_BOOT_0", "Chip ID / Architecture"),
        (0x00000188, "NV_PMC_BOOT_42", "Extended boot info"),
    ],

    # --- PMC (Power Management Controller) ---
    "pmc": [
        (0x00000200, "NV_PMC_ENABLE", "Device enable mask"),
        (0x00000600, "NV_PMC_DEVICE_ENABLE", "Device enable"),
        (0x00000140, "NV_PMC_INTR_EN_0", "Interrupt enable"),
    ],

    # --- PBUS (Bus Control) ---
    "pbus": [
        (0x00001400, "NV_PBUS_VBIOS_SCRATCH", "VBIOS scratch"),
        (0x00001438, "NV_PBUS_VBIOS_SCRATCH_FRTS_ERR", "FRTS error (upper 16 bits = error)"),
        (0x00001454, "NV_PBUS_VBIOS_SCRATCH_FWSEC_ERR", "FWSEC error (lower 16 bits = error)"),
        (0x0000184C, "NV_PBUS_PCI_NV_19", "BAR0 state"),
    ],

    # --- Memory Controller (WPR2) ---
    "pfb": [
        (0x00100C80, "NV_PFB_PRI_MMU_CTRL", "MMU control"),
        (0x001FA824, "NV_PFB_PRI_MMU_WPR2_ADDR_LO", "WPR2 low address"),
        (0x001FA828, "NV_PFB_PRI_MMU_WPR2_ADDR_HI", "WPR2 high address"),
    ],

    # --- GSP Falcon ---
    "gsp_falcon": [
        (0x00110040, "NV_PGSP_FALCON_MAILBOX0", "GSP Falcon Mailbox 0"),
        (0x00110044, "NV_PGSP_FALCON_MAILBOX1", "GSP Falcon Mailbox 1"),
        (0x00110100, "NV_PGSP_FALCON_CPUCTL", "GSP Falcon CPUCTL (bit4=halted)"),
        (0x0011004C, "NV_PGSP_FALCON_IDLESTATE", "GSP Falcon idle state"),
        (0x00110108, "NV_PGSP_FALCON_HWCFG", "GSP Falcon HWCFG"),
    ],

    # --- GSP RISC-V ---
    "gsp_riscv": [
        (0x00118388, "NV_PRISCV_RISCV_CPUCTL", "GSP RISC-V CPUCTL (bit4=halted, bit7=active)"),
        (0x00118668, "NV_PRISCV_RISCV_BCR_CTRL", "GSP RISC-V BCR_CTRL (bit0=valid)"),
        (0x00118400, "NV_PRISCV_RISCV_BR_RETCODE", "GSP RISC-V boot return code"),
        (0x00118544, "NV_PRISCV_RISCV_CORE_HALT", "GSP RISC-V core halt"),
        (0x00118F58, "NV_PGC6_BSI_SECURE_SCRATCH_14", "GSP boot stage scratch"),
    ],

    # --- SEC2 Falcon ---
    "sec2_falcon": [
        (0x00840040, "NV_PSEC_FALCON_MAILBOX0", "SEC2 Falcon Mailbox 0"),
        (0x00840044, "NV_PSEC_FALCON_MAILBOX1", "SEC2 Falcon Mailbox 1"),
        (0x00840100, "NV_PSEC_FALCON_CPUCTL", "SEC2 Falcon CPUCTL"),
    ],

    # --- SEC2 RISC-V ---
    "sec2_riscv": [
        (0x00841388, "NV_PSEC_RISCV_CPUCTL", "SEC2 RISC-V CPUCTL"),
        (0x00841400, "NV_PSEC_RISCV_BR_RETCODE", "SEC2 RISC-V boot return code"),
        (0x00841668, "NV_PSEC_RISCV_BCR_CTRL", "SEC2 RISC-V BCR_CTRL"),
    ],

    # --- Timer ---
    "timer": [
        (0x00009400, "NV_PTIMER_TIME_0", "Timer low 32 bits"),
        (0x00009410, "NV_PTIMER_TIME_1", "Timer high 32 bits"),
    ],

    # --- Compute Engine ---
    "compute": [
        (0x00104040, "NV_PCE_FALCON_MAILBOX0", "Compute Engine Falcon Mailbox 0"),
    ],

    # --- GSP Queue Registers ---
    "gsp_queue": [
        (0x00110C00, "NV_PGSP_QUEUE_HEAD[0]", "GSP cmd queue head"),
        (0x00110C08, "NV_PGSP_QUEUE_HEAD[1]", "GSP msg queue head"),
        (0x00110C80, "NV_PGSP_QUEUE_TAIL[0]", "GSP cmd queue tail"),
        (0x00110C88, "NV_PGSP_QUEUE_TAIL[1]", "GSP msg queue tail"),
    ],

    # --- Fuse ---
    "fuse": [
        (0x008241C0, "NV_FUSE_OPT_FPF_GSP_UCODE1_VERSION", "GSP ucode1 fuse version"),
    ],
}

# Architecture names
ARCH_NAMES = {
    0x13: "Kepler",
    0x14: "Maxwell",
    0x15: "Maxwell v2",
    0x16: "Pascal",
    0x17: "Volta/Ampere (GA1xx)",
    0x18: "Turing",
    0x19: "Ada Lovelace (AD1xx)",
    0x1A: "Hopper",
    0x1B: "Blackwell (GB2xx)",
}

# Known device IDs
DEVICE_IDS = {
    0x2684: "RTX 4090",
    0x2685: "RTX 4090 (variant)",
    0x2702: "RTX 4080",
    0x2704: "RTX 4080 (variant)",
    0x2705: "RTX 4070 Ti",
    0x2782: "RTX 4070 Ti (variant)",
    0x2786: "RTX 4070",
    0x2860: "RTX 4070 (variant)",
}


# ============================================================================
# Helper Functions
# ============================================================================

def read_reg32(mm, offset):
    """Read a 32-bit register from mmap'd BAR0."""
    if offset + 4 > BAR0_SIZE:
        return None
    mm.seek(offset)
    data = mm.read(4)
    if len(data) < 4:
        return None
    return struct.unpack('<I', data)[0]


def decode_pmc_boot_0(val):
    """Decode NV_PMC_BOOT_0 register.

    NVIDIA convention (from open-gpu-kernel-modules):
    - Bits 31:24 = Architecture (0x19 = Ada Lovelace)
    - Bits 23:20 = Implementation (0x2 = AD102, 0x4 = AD104, etc.)
    - Bits 19:16 = (reserved/variant)
    - Bits 15:8  = (reserved)
    - Bits  7:4  = Major revision
    - Bits  3:0  = Minor revision
    - Bits 31:20 = Full chip ID (e.g., 0x192 = AD102)

    Note: The NVDAALRegs.h header says 'bits 24:20' but NVIDIA's actual
    architecture field is bits 31:24 (full byte). The chip ID is bits 31:20.
    """
    lines = []
    arch = (val >> 24) & 0xFF       # bits 31:24 = architecture generation
    impl = (val >> 20) & 0xF        # bits 23:20 = implementation within arch
    chip_id = (val >> 20) & 0xFFF   # bits 31:20 = full chip ID
    major_rev = (val >> 4) & 0xF    # bits 7:4
    minor_rev = val & 0xF           # bits 3:0

    arch_name = ARCH_NAMES.get(arch, f"Unknown (0x{arch:02X})")
    lines.append(f"    Architecture [31:24]  : 0x{arch:02X} = {arch_name}")
    lines.append(f"    Implementation [23:20]: 0x{impl:X}")
    lines.append(f"    Chip ID [31:20]       : 0x{chip_id:03X}")
    lines.append(f"    Major revision [7:4]  : {major_rev}")
    lines.append(f"    Minor revision [3:0]  : {minor_rev}")

    # Decode known chip IDs
    known_chips = {
        0x192: "AD102 (RTX 4090)",
        0x194: "AD104 (RTX 4070 Ti)",
        0x196: "AD106 (RTX 4070)",
        0x197: "AD107 (RTX 4060)",
    }
    chip_name = known_chips.get(chip_id, "Unknown")
    lines.append(f"    Chip                  : {chip_name}")

    if arch == 0x19:
        lines.append(f"    ** CONFIRMED: Ada Lovelace architecture **")
    else:
        lines.append(f"    ** WARNING: Expected Ada (0x19), got 0x{arch:02X} **")

    return lines


def decode_pmc_boot_42(val):
    """Decode NV_PMC_BOOT_42 register."""
    lines = []
    lines.append(f"    Raw extended boot info: 0x{val:08X}")
    # Extract subfields (architecture-dependent)
    lines.append(f"    Bits [31:24]: 0x{(val >> 24) & 0xFF:02X}")
    lines.append(f"    Bits [23:16]: 0x{(val >> 16) & 0xFF:02X}")
    lines.append(f"    Bits [15:8] : 0x{(val >> 8) & 0xFF:02X}")
    lines.append(f"    Bits [7:0]  : 0x{val & 0xFF:02X}")
    return lines


def decode_cpuctl(val, engine_name):
    """Decode a FALCON CPUCTL register."""
    lines = []
    halted = (val >> 4) & 1
    started = (val >> 1) & 1
    lines.append(f"    Halted [bit 4]  : {'YES' if halted else 'NO'}")
    lines.append(f"    Started [bit 1] : {'YES' if started else 'NO'}")
    if engine_name.startswith("GSP RISC-V") or engine_name.startswith("SEC2 RISC-V"):
        active = (val >> 7) & 1
        lines.append(f"    Active [bit 7]  : {'YES' if active else 'NO'}")
    return lines


def decode_bcr_ctrl(val):
    """Decode BCR_CTRL register."""
    lines = []
    valid = val & 1
    core_select = (val >> 4) & 1
    lines.append(f"    Valid [bit 0]       : {'YES' if valid else 'NO'}")
    lines.append(f"    Core select [bit 4] : {'RISC-V' if core_select else 'Falcon'}")
    return lines


def decode_wpr2_addr(val, which):
    """Decode WPR2 address register."""
    lines = []
    enabled = (val >> 31) & 1
    addr = val & 0x7FFFFFFF
    # WPR2 address is in 256-byte units (shift left 8 for actual address)
    actual_addr = addr << 8
    lines.append(f"    Enabled [bit 31] : {'YES' if enabled else 'NO'}")
    lines.append(f"    Address [30:0]   : 0x{addr:08X} (raw)")
    lines.append(f"    Actual address   : 0x{actual_addr:012X} (shifted << 8)")
    return lines


def decode_frts_err(val):
    """Decode FRTS error register (scratch 0x0E)."""
    lines = []
    err = (val >> 16) & 0xFFFF
    lower = val & 0xFFFF
    lines.append(f"    FRTS error [31:16]  : 0x{err:04X} ({'SUCCESS' if err == 0 else f'ERROR code {err}'})")
    lines.append(f"    Lower bits [15:0]   : 0x{lower:04X}")
    return lines


def decode_fwsec_err(val):
    """Decode FWSEC error register (scratch 0x15)."""
    lines = []
    err = val & 0xFFFF
    upper = (val >> 16) & 0xFFFF
    lines.append(f"    FWSEC error [15:0]  : 0x{err:04X} ({'SUCCESS' if err == 0 else f'ERROR code {err}'})")
    lines.append(f"    Upper bits [31:16]  : 0x{upper:04X}")
    return lines


def decode_bar0_state(val):
    """Decode BAR0 state register."""
    lines = []
    active = val & 1
    lines.append(f"    BAR0 active [bit 0] : {'YES' if active else 'NO'}")
    lines.append(f"    Full value          : 0x{val:08X}")
    return lines


def decode_boot_stage(val):
    """Decode boot stage scratch register."""
    lines = []
    lines.append(f"    Boot stage value    : 0x{val:08X}")
    if val == 0x3:
        lines.append(f"    ** Boot stage 3: HANDOFF complete **")
    elif val == 0:
        lines.append(f"    ** Boot stage 0: Not started / reset **")
    else:
        lines.append(f"    ** Boot stage: {val} **")
    return lines


def decode_br_retcode(val):
    """Decode boot ROM return code."""
    lines = []
    retcode = val & 0xFF
    status = (val >> 8) & 0xFF
    lines.append(f"    Return code [7:0]   : 0x{retcode:02X}")
    lines.append(f"    Status [15:8]       : 0x{status:02X}")
    lines.append(f"    Full value          : 0x{val:08X}")
    return lines


def decode_gsp_queue(val, which):
    """Decode GSP queue register."""
    lines = []
    lines.append(f"    Offset value        : 0x{val:08X} ({val} bytes)")
    return lines


def decode_timer(lo, hi):
    """Decode timer registers."""
    lines = []
    full = (hi << 32) | lo
    # Timer is in nanoseconds
    seconds = full / 1e9
    lines.append(f"    Full 64-bit value   : 0x{full:016X}")
    lines.append(f"    Time (ns)           : {full}")
    lines.append(f"    Time (seconds)      : {seconds:.3f}")
    lines.append(f"    Time (minutes)      : {seconds/60:.2f}")
    return lines


def decode_idle_state(val):
    """Decode Falcon idle state."""
    lines = []
    idle = val & 1
    lines.append(f"    Idle [bit 0]        : {'YES (idle)' if idle else 'NO (busy)'}")
    lines.append(f"    Full value          : 0x{val:08X}")
    return lines


def decode_mmu_ctrl(val):
    """Decode MMU control register."""
    lines = []
    lines.append(f"    Full value          : 0x{val:08X}")
    lines.append(f"    Bits [3:0]          : 0x{val & 0xF:X}")
    lines.append(f"    Bits [7:4]          : 0x{(val >> 4) & 0xF:X}")
    return lines


# ============================================================================
# Main Script
# ============================================================================

def main():
    output_lines = []

    def out(s=""):
        output_lines.append(s)
        print(s)

    out("=" * 80)
    out("NVIDIA RTX 4090 (AD102) - BAR0 MMIO Register Dump")
    out(f"PCI Device: {PCI_DEVICE}")
    out(f"Resource:   {RESOURCE0_PATH}")
    out(f"Timestamp:  {time.strftime('%Y-%m-%d %H:%M:%S %Z')}")
    out("=" * 80)

    # Check privileges
    if os.geteuid() != 0:
        out("\nERROR: Must run as root (sudo). Exiting.")
        sys.exit(1)

    # Check resource exists
    if not os.path.exists(RESOURCE0_PATH):
        out(f"\nERROR: {RESOURCE0_PATH} not found. Exiting.")
        sys.exit(1)

    # Open and mmap BAR0
    try:
        fd = os.open(RESOURCE0_PATH, os.O_RDONLY | os.O_SYNC)
        mm = mmap.mmap(fd, BAR0_SIZE, mmap.MAP_SHARED, mmap.PROT_READ)
        out(f"\nSuccessfully mmap'd BAR0 ({BAR0_SIZE // 1024 // 1024}MB)")
    except Exception as e:
        out(f"\nERROR: Failed to mmap BAR0: {e}")
        sys.exit(1)

    # Store timer values for combined decode
    timer_lo = None
    timer_hi = None

    # Read all register groups
    for group_name, regs in REGISTERS.items():
        section_title = {
            "chip_id": "CHIP IDENTIFICATION",
            "pmc": "PMC (Power Management Controller)",
            "pbus": "PBUS (Bus Control)",
            "pfb": "PFB (Memory Controller / WPR2)",
            "gsp_falcon": "GSP FALCON",
            "gsp_riscv": "GSP RISC-V",
            "sec2_falcon": "SEC2 FALCON",
            "sec2_riscv": "SEC2 RISC-V",
            "timer": "TIMER",
            "compute": "COMPUTE ENGINE",
            "gsp_queue": "GSP QUEUE REGISTERS",
            "fuse": "FUSE REGISTERS",
        }.get(group_name, group_name.upper())

        out(f"\n{'─' * 80}")
        out(f"  {section_title}")
        out(f"{'─' * 80}")

        for offset, name, desc in regs:
            val = read_reg32(mm, offset)
            if val is None:
                out(f"\n  [0x{offset:08X}] {name}")
                out(f"  Description: {desc}")
                out(f"  Value: READ FAILED (offset out of range)")
                continue

            out(f"\n  [0x{offset:08X}] {name}")
            out(f"  Description: {desc}")
            out(f"  Value: 0x{val:08X} ({val})")

            # Decode specific registers
            if name == "NV_PMC_BOOT_0":
                for line in decode_pmc_boot_0(val):
                    out(line)

            elif name == "NV_PMC_BOOT_42":
                for line in decode_pmc_boot_42(val):
                    out(line)

            elif name == "NV_PBUS_VBIOS_SCRATCH_FRTS_ERR":
                for line in decode_frts_err(val):
                    out(line)

            elif name == "NV_PBUS_VBIOS_SCRATCH_FWSEC_ERR":
                for line in decode_fwsec_err(val):
                    out(line)

            elif name == "NV_PBUS_PCI_NV_19":
                for line in decode_bar0_state(val):
                    out(line)

            elif name == "NV_PFB_PRI_MMU_CTRL":
                for line in decode_mmu_ctrl(val):
                    out(line)

            elif "WPR2_ADDR_LO" in name:
                for line in decode_wpr2_addr(val, "LO"):
                    out(line)

            elif "WPR2_ADDR_HI" in name:
                for line in decode_wpr2_addr(val, "HI"):
                    out(line)

            elif "CPUCTL" in name and "BCR" not in name:
                engine = ""
                if "PGSP" in name or "PRISCV" in name:
                    engine = "GSP RISC-V" if "PRISCV" in name else "GSP Falcon"
                elif "PSEC" in name:
                    engine = "SEC2 RISC-V" if "RISCV" in name else "SEC2 Falcon"
                for line in decode_cpuctl(val, engine):
                    out(line)

            elif "BCR_CTRL" in name:
                for line in decode_bcr_ctrl(val):
                    out(line)

            elif "SECURE_SCRATCH_14" in name:
                for line in decode_boot_stage(val):
                    out(line)

            elif "BR_RETCODE" in name:
                for line in decode_br_retcode(val):
                    out(line)

            elif "IDLESTATE" in name:
                for line in decode_idle_state(val):
                    out(line)

            elif "QUEUE_HEAD" in name or "QUEUE_TAIL" in name:
                for line in decode_gsp_queue(val, name):
                    out(line)

            elif name == "NV_PTIMER_TIME_0":
                timer_lo = val

            elif name == "NV_PTIMER_TIME_1":
                timer_hi = val

    # Print combined timer decode
    if timer_lo is not None and timer_hi is not None:
        out(f"\n  Combined Timer Value:")
        for line in decode_timer(timer_lo, timer_hi):
            out(line)

    # ========================================================================
    # Additional Analysis
    # ========================================================================
    out(f"\n{'=' * 80}")
    out("  ANALYSIS SUMMARY")
    out(f"{'=' * 80}")

    # Re-read key registers for summary
    boot0 = read_reg32(mm, 0x00000000)
    if boot0 is not None:
        arch = (boot0 >> 24) & 0xFF     # bits 31:24 = architecture
        chip_id = (boot0 >> 20) & 0xFFF # bits 31:20 = full chip ID
        arch_name = ARCH_NAMES.get(arch, "Unknown")
        out(f"\n  GPU: Chip 0x{chip_id:03X}, Architecture: {arch_name}")

    frts = read_reg32(mm, 0x00001438)
    if frts is not None:
        frts_err = (frts >> 16) & 0xFFFF
        out(f"  FRTS Status: {'OK (FWSEC-FRTS completed successfully)' if frts_err == 0 else f'ERROR 0x{frts_err:04X}'}")

    fwsec = read_reg32(mm, 0x00001454)
    if fwsec is not None:
        fwsec_err = fwsec & 0xFFFF
        out(f"  FWSEC Status: {'OK' if fwsec_err == 0 else f'ERROR 0x{fwsec_err:04X}'}")

    wpr2_lo = read_reg32(mm, 0x001FA824)
    wpr2_hi = read_reg32(mm, 0x001FA828)
    if wpr2_lo is not None and wpr2_hi is not None:
        wpr2_lo_en = (wpr2_lo >> 31) & 1
        wpr2_hi_en = (wpr2_hi >> 31) & 1
        if wpr2_lo_en and wpr2_hi_en:
            lo_addr = (wpr2_lo & 0x7FFFFFFF) << 8
            hi_addr = (wpr2_hi & 0x7FFFFFFF) << 8
            wpr2_size = hi_addr - lo_addr
            out(f"  WPR2: ENABLED, range 0x{lo_addr:012X} - 0x{hi_addr:012X} (size: {wpr2_size / 1024 / 1024:.1f}MB)")
        else:
            out(f"  WPR2: NOT ENABLED (lo_en={wpr2_lo_en}, hi_en={wpr2_hi_en})")

    gsp_cpuctl = read_reg32(mm, 0x00118388)
    if gsp_cpuctl is not None:
        halted = (gsp_cpuctl >> 4) & 1
        active = (gsp_cpuctl >> 7) & 1
        if active and not halted:
            out(f"  GSP RISC-V: RUNNING (active, not halted)")
        elif halted:
            out(f"  GSP RISC-V: HALTED")
        else:
            out(f"  GSP RISC-V: INACTIVE (not active, not halted)")

    sec2_cpuctl = read_reg32(mm, 0x00841388)
    if sec2_cpuctl is not None:
        halted = (sec2_cpuctl >> 4) & 1
        active = (sec2_cpuctl >> 7) & 1
        if active and not halted:
            out(f"  SEC2 RISC-V: RUNNING (active, not halted)")
        elif halted:
            out(f"  SEC2 RISC-V: HALTED")
        else:
            out(f"  SEC2 RISC-V: INACTIVE")

    boot_stage = read_reg32(mm, 0x00118F58)
    if boot_stage is not None:
        out(f"  Boot stage: 0x{boot_stage:X}" +
            (" (HANDOFF complete)" if boot_stage == 3 else ""))

    # ========================================================================
    # VBIOS via PROM space
    # ========================================================================
    out(f"\n{'=' * 80}")
    out("  VBIOS via PROM Space (BAR0 + 0x180000)")
    out(f"{'=' * 80}")

    try:
        # Read first 256 bytes from PROM space
        prom_header = bytearray()
        for i in range(0, 256, 4):
            val = read_reg32(mm, NV_PROM_BASE + i)
            if val is not None:
                prom_header.extend(struct.pack('<I', val))
            else:
                prom_header.extend(b'\x00\x00\x00\x00')

        out(f"\n  PROM first 64 bytes (hex):")
        for i in range(0, 64, 16):
            hex_str = ' '.join(f'{b:02X}' for b in prom_header[i:i+16])
            ascii_str = ''.join(chr(b) if 32 <= b < 127 else '.' for b in prom_header[i:i+16])
            out(f"    {i:04X}: {hex_str}  |{ascii_str}|")

        # Check ROM signature
        sig = struct.unpack_from('<H', prom_header, 0)[0]
        if sig == 0xAA55:
            out(f"\n  ROM signature: 0x{sig:04X} (VALID - standard PCI ROM)")
            # Get PCIR offset
            pcir_off = struct.unpack_from('<H', prom_header, 0x18)[0]
            out(f"  PCIR pointer at offset 0x18: 0x{pcir_off:04X}")

            if pcir_off < 256:
                pcir_sig = struct.unpack_from('<I', prom_header, pcir_off)[0]
                if pcir_sig == 0x52494350:  # "PCIR"
                    vendor = struct.unpack_from('<H', prom_header, pcir_off + 4)[0]
                    device = struct.unpack_from('<H', prom_header, pcir_off + 6)[0]
                    img_len = struct.unpack_from('<H', prom_header, pcir_off + 0x10)[0]
                    code_type = prom_header[pcir_off + 0x14]
                    indicator = prom_header[pcir_off + 0x15]
                    out(f"  PCIR signature: VALID")
                    out(f"  Vendor ID: 0x{vendor:04X}" + (" (NVIDIA)" if vendor == 0x10DE else ""))
                    device_name = DEVICE_IDS.get(device, "Unknown")
                    out(f"  Device ID: 0x{device:04X} ({device_name})")
                    out(f"  Image length: {img_len} x 512 = {img_len * 512} bytes")
                    code_type_names = {0x00: "PCI/AT", 0x03: "EFI", 0xE0: "FWSEC"}
                    out(f"  Code type: 0x{code_type:02X} ({code_type_names.get(code_type, 'Unknown')})")
                    out(f"  Indicator: 0x{indicator:02X} (last={'YES' if indicator & 0x80 else 'NO'})")
                else:
                    out(f"  PCIR signature at 0x{pcir_off:04X}: 0x{pcir_sig:08X} (INVALID, expected 'PCIR')")
        elif sig == 0x0000:
            out(f"\n  ROM signature: 0x{sig:04X} (EMPTY/ZERO - PROM may not be accessible)")
        else:
            out(f"\n  ROM signature: 0x{sig:04X} (UNEXPECTED)")

        # Compare with sysfs ROM dump if available
        out(f"\n  Comparing with sysfs ROM dump...")
        vbios_path = "/home/gabriel-maia/Documentos/NVDAAL-Driver/ReverseEng/vbios_raw.rom"
        if os.path.exists(vbios_path):
            with open(vbios_path, 'rb') as f:
                sysfs_header = f.read(64)

            out(f"\n  Sysfs ROM first 64 bytes:")
            for i in range(0, min(64, len(sysfs_header)), 16):
                hex_str = ' '.join(f'{b:02X}' for b in sysfs_header[i:i+16])
                ascii_str = ''.join(chr(b) if 32 <= b < 127 else '.' for b in sysfs_header[i:i+16])
                out(f"    {i:04X}: {hex_str}  |{ascii_str}|")

            # Compare
            prom_first64 = bytes(prom_header[:64])
            sysfs_first64 = sysfs_header[:64]
            if prom_first64 == sysfs_first64:
                out(f"\n  MATCH: PROM and sysfs ROM first 64 bytes are IDENTICAL")
            else:
                diffs = sum(1 for a, b in zip(prom_first64, sysfs_first64) if a != b)
                out(f"\n  MISMATCH: {diffs} bytes differ in first 64 bytes")
                out(f"  (PROM space may be gated/encrypted or read differently)")

            # Also compare sysfs ROM signature
            sysfs_sig = struct.unpack_from('<H', sysfs_header, 0)[0]
            out(f"  Sysfs ROM signature: 0x{sysfs_sig:04X}" +
                (" (VALID)" if sysfs_sig == 0xAA55 else " (INVALID)"))
        else:
            out(f"  No sysfs ROM dump found at {vbios_path}")

        # Also try reading sysfs rom directly
        if os.path.exists(ROM_PATH):
            out(f"\n  Sysfs ROM device: {ROM_PATH}")
            try:
                # Enable ROM reading
                with open(ROM_PATH, 'wb') as f:
                    f.write(b'1')
                with open(ROM_PATH, 'rb') as f:
                    sysfs_rom_data = f.read(64)
                # Disable ROM reading
                with open(ROM_PATH, 'wb') as f:
                    f.write(b'0')

                out(f"  Sysfs ROM (live) first 64 bytes:")
                for i in range(0, min(64, len(sysfs_rom_data)), 16):
                    hex_str = ' '.join(f'{b:02X}' for b in sysfs_rom_data[i:i+16])
                    ascii_str = ''.join(chr(b) if 32 <= b < 127 else '.' for b in sysfs_rom_data[i:i+16])
                    out(f"    {i:04X}: {hex_str}  |{ascii_str}|")

                live_sig = struct.unpack_from('<H', sysfs_rom_data, 0)[0]
                out(f"  Sysfs ROM (live) signature: 0x{live_sig:04X}" +
                    (" (VALID)" if live_sig == 0xAA55 else ""))

                # Compare PROM vs sysfs live
                if bytes(prom_header[:64]) == sysfs_rom_data[:64]:
                    out(f"  PROM vs sysfs ROM (live): IDENTICAL")
                else:
                    diffs = sum(1 for a, b in zip(prom_header[:64], sysfs_rom_data[:64]) if a != b)
                    out(f"  PROM vs sysfs ROM (live): {diffs} byte(s) differ")

            except PermissionError:
                out(f"  Cannot access sysfs ROM (permission denied)")
            except Exception as e:
                out(f"  Error reading sysfs ROM: {e}")
        else:
            out(f"  Sysfs ROM device not available at {ROM_PATH}")

    except Exception as e:
        out(f"\n  Error reading PROM space: {e}")

    # ========================================================================
    # Extra: Read additional PROM regions to find all ROM images
    # ========================================================================
    out(f"\n{'=' * 80}")
    out("  VBIOS ROM IMAGE WALK (via PROM)")
    out(f"{'=' * 80}")

    try:
        rom_offset = 0
        image_num = 0
        while rom_offset < VBIOS_MAX_SIZE:
            # Read ROM header at current offset
            sig_val = read_reg32(mm, NV_PROM_BASE + rom_offset)
            if sig_val is None:
                break

            sig = sig_val & 0xFFFF
            if sig != 0xAA55:
                if image_num == 0:
                    out(f"\n  No valid ROM images found in PROM space (first word: 0x{sig_val:08X})")
                break

            image_num += 1

            # Read PCIR offset
            pcir_ptr_val = read_reg32(mm, NV_PROM_BASE + rom_offset + 0x18)
            if pcir_ptr_val is None:
                break
            pcir_off = pcir_ptr_val & 0xFFFF

            # Read PCIR data
            pcir_abs = NV_PROM_BASE + rom_offset + pcir_off
            pcir_data = bytearray()
            for i in range(0, 32, 4):
                v = read_reg32(mm, pcir_abs + i)
                if v is not None:
                    pcir_data.extend(struct.pack('<I', v))

            if len(pcir_data) >= 22:
                pcir_sig = struct.unpack_from('<I', pcir_data, 0)[0]
                if pcir_sig == 0x52494350:  # "PCIR"
                    vendor = struct.unpack_from('<H', pcir_data, 4)[0]
                    device = struct.unpack_from('<H', pcir_data, 6)[0]
                    img_len = struct.unpack_from('<H', pcir_data, 0x10)[0]
                    code_type = pcir_data[0x14]
                    indicator = pcir_data[0x15]

                    code_type_names = {0x00: "PCI/AT BIOS", 0x03: "UEFI GOP", 0xE0: "FWSEC"}
                    device_name = DEVICE_IDS.get(device, "Unknown")

                    out(f"\n  Image #{image_num} at PROM offset 0x{rom_offset:06X}:")
                    out(f"    Vendor: 0x{vendor:04X}, Device: 0x{device:04X} ({device_name})")
                    out(f"    Code type: 0x{code_type:02X} ({code_type_names.get(code_type, 'Unknown')})")
                    out(f"    Image size: {img_len * 512} bytes ({img_len} x 512)")
                    out(f"    Last image: {'YES' if indicator & 0x80 else 'NO'}")

                    # Advance to next image
                    next_offset = rom_offset + img_len * 512
                    if indicator & 0x80:
                        # Last image
                        out(f"    (Last image in chain)")
                        break
                    rom_offset = next_offset
                else:
                    out(f"\n  Image #{image_num} at 0x{rom_offset:06X}: Invalid PCIR (0x{pcir_sig:08X})")
                    break
            else:
                out(f"\n  Image #{image_num} at 0x{rom_offset:06X}: Could not read PCIR data")
                break

    except Exception as e:
        out(f"\n  Error walking ROM images: {e}")

    # ========================================================================
    # Cleanup
    # ========================================================================
    mm.close()
    os.close(fd)

    out(f"\n{'=' * 80}")
    out(f"  Dump complete.")
    out(f"{'=' * 80}")

    return '\n'.join(output_lines)


if __name__ == '__main__':
    result = main()

    # Save to file
    output_path = "/home/gabriel-maia/Documentos/NVDAAL-Driver/ReverseEng/register_dump.txt"
    with open(output_path, 'w') as f:
        f.write(result + '\n')
    print(f"\nOutput saved to: {output_path}")
