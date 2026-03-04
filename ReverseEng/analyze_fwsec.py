#!/usr/bin/env python3
"""
Deep VBIOS FWSEC Analysis for NVIDIA RTX 4090 (AD102)
Analyzes the full 988KB VBIOS ROM to extract FWSEC firmware information.
"""

import struct
import sys
import os
from datetime import datetime

ROM_PATH = "/home/gabriel-maia/Documentos/NVDAAL-Driver/ReverseEng/vbios_full.rom"
OUTPUT_PATH = "/home/gabriel-maia/Documentos/NVDAAL-Driver/ReverseEng/vbios_fwsec_analysis.txt"

# Known AppIDs from NVIDIA firmware
APP_ID_NAMES = {
    0x01: "PMU",
    0x04: "DISP",
    0x07: "SEC2",
    0x0A: "GSP",
    0x45: "FWSEC_DBG",
    0x85: "FWSEC",
}

# Known BIT token types
BIT_TOKEN_NAMES = {
    0x42: "BIOSDATA (B)",
    0x43: "CLOCK (C)",
    0x44: "DFP (D)",
    0x46: "FALCON (F)",  # Actually token ID, but 0x46='F'
    0x49: "INIT (I)",
    0x4D: "MEMORY (M)",
    0x4E: "NOP (N)",
    0x50: "PERF (P)",
    0x52: "RAM_CFG (R)",
    0x53: "STRING (S)",
    0x54: "TMDS (T)",
    0x55: "USB (U)",
    0x56: "VIRTUAL (V)",
    0x58: "MXM (X)",
    0x70: "FALCON_DATA",
    0x32: "DEVINIT_2",
}

out_lines = []

def log(msg=""):
    out_lines.append(msg)
    print(msg)

def hexdump(data, offset=0, length=None, prefix="  "):
    """Produce a hex dump of data."""
    if length is None:
        length = len(data)
    lines = []
    for i in range(0, min(length, len(data)), 16):
        hex_part = " ".join(f"{data[i+j]:02X}" if i+j < min(length, len(data)) else "  " for j in range(16))
        ascii_part = "".join(chr(data[i+j]) if 32 <= data[i+j] < 127 else "." for j in range(min(16, min(length, len(data)) - i)))
        lines.append(f"{prefix}{offset+i:08X}: {hex_part}  {ascii_part}")
    return "\n".join(lines)

def read_u8(rom, off):
    return struct.unpack_from("<B", rom, off)[0]

def read_u16(rom, off):
    return struct.unpack_from("<H", rom, off)[0]

def read_u32(rom, off):
    return struct.unpack_from("<I", rom, off)[0]

def read_u64(rom, off):
    return struct.unpack_from("<Q", rom, off)[0]

def safe_string(rom, off, maxlen=64):
    """Read a null-terminated string."""
    s = []
    for i in range(maxlen):
        if off + i >= len(rom):
            break
        b = rom[off + i]
        if b == 0:
            break
        if 32 <= b < 127:
            s.append(chr(b))
        else:
            break
    return "".join(s)

def find_all(rom, pattern):
    """Find all occurrences of a byte pattern."""
    results = []
    start = 0
    while True:
        idx = rom.find(pattern, start)
        if idx == -1:
            break
        results.append(idx)
        start = idx + 1
    return results


def analyze_pci_rom_headers(rom):
    """Scan all PCI ROM images (0xAA55 signatures)."""
    log("=" * 80)
    log("SECTION 1: PCI ROM IMAGE SCAN")
    log("=" * 80)
    log()

    images = []
    offset = 0
    img_num = 0

    while offset < len(rom) - 2:
        sig = read_u16(rom, offset)
        if sig != 0xAA55:
            # Try next 512-byte boundary
            offset += 0x200
            continue

        img_num += 1
        # Find PCIR structure
        pcir_off_field = read_u16(rom, offset + 0x18)
        pcir_abs = offset + pcir_off_field

        pcir_sig = rom[pcir_abs:pcir_abs+4]
        if pcir_sig != b'PCIR':
            log(f"  Image #{img_num} at 0x{offset:06X}: AA55 found but no PCIR at +0x{pcir_off_field:04X}")
            offset += 0x200
            continue

        vendor_id = read_u16(rom, pcir_abs + 4)
        device_id = read_u16(rom, pcir_abs + 6)
        pcir_len = read_u16(rom, pcir_abs + 10)
        pcir_rev = read_u8(rom, pcir_abs + 12)
        class_code = (read_u8(rom, pcir_abs + 15) << 16) | (read_u8(rom, pcir_abs + 14) << 8) | read_u8(rom, pcir_abs + 13)
        image_len = read_u16(rom, pcir_abs + 16) * 512
        code_rev = read_u16(rom, pcir_abs + 18)
        code_type = read_u8(rom, pcir_abs + 20)
        indicator = read_u8(rom, pcir_abs + 21)
        max_len = read_u16(rom, pcir_abs + 22) * 512

        code_type_name = {0x00: "x86/AT", 0x01: "Open Firmware", 0x03: "EFI", 0xE0: "NVIDIA FWSEC"}.get(code_type, f"Unknown(0x{code_type:02X})")
        last_image = (indicator & 0x80) != 0

        img_info = {
            "offset": offset,
            "size": image_len,
            "code_type": code_type,
            "code_type_name": code_type_name,
            "vendor_id": vendor_id,
            "device_id": device_id,
            "last": last_image,
        }
        images.append(img_info)

        log(f"  Image #{img_num}:")
        log(f"    Offset:       0x{offset:06X} - 0x{offset + image_len:06X} ({image_len} bytes / {image_len/1024:.1f} KB)")
        log(f"    PCIR at:      0x{pcir_abs:06X} (offset +0x{pcir_off_field:04X})")
        log(f"    Vendor:       0x{vendor_id:04X}  Device: 0x{device_id:04X}")
        log(f"    Code Type:    0x{code_type:02X} ({code_type_name})")
        log(f"    Code Rev:     {code_rev}")
        log(f"    PCIR Rev:     {pcir_rev}")
        log(f"    Class Code:   0x{class_code:06X}")
        log(f"    Image Length: {image_len} bytes (0x{image_len:X})")
        log(f"    Max Length:   {max_len} bytes (0x{max_len:X})")
        log(f"    Last Image:   {last_image}")
        log(f"    Raw PCIR:")
        log(hexdump(rom[pcir_abs:pcir_abs+24], pcir_abs, prefix="      "))
        log()

        # Check for NPDE (NVIDIA PCI Data Extension)
        npde_off_field_pos = offset + 0x1A
        if npde_off_field_pos + 2 <= len(rom):
            npde_off_field = read_u16(rom, npde_off_field_pos)
            if npde_off_field > 0 and npde_off_field < image_len:
                npde_abs = offset + npde_off_field
                npde_sig = rom[npde_abs:npde_abs+4]
                if npde_sig == b'NPDE':
                    npde_rev = read_u8(rom, npde_abs + 4)
                    npde_len = read_u8(rom, npde_abs + 5)
                    npde_sub_img_len = read_u16(rom, npde_abs + 6) * 512
                    npde_last_sub = read_u8(rom, npde_abs + 8)
                    log(f"    NPDE at 0x{npde_abs:06X}:")
                    log(f"      Revision:        {npde_rev}")
                    log(f"      Length:          {npde_len}")
                    log(f"      Sub-Image Len:   {npde_sub_img_len} bytes")
                    log(f"      Last Sub-Image:  {npde_last_sub}")
                    log(hexdump(rom[npde_abs:npde_abs+16], npde_abs, prefix="      "))
                    log()

        offset += image_len
        if last_image:
            break

    log(f"  Total PCI ROM images found: {len(images)}")
    log()
    return images


def analyze_bit_header(rom):
    """Parse the BIT (BIOS Information Table) header and all tokens."""
    log("=" * 80)
    log("SECTION 2: BIT HEADER AND TOKENS (Full Re-analysis)")
    log("=" * 80)
    log()

    # Find BIT signature (0x00FF 'B' 'I' 'T')
    bit_sig = b'\x00\xFFBIT'
    bit_offsets = find_all(rom, bit_sig)

    if not bit_offsets:
        # Try alternate: look for just "BIT" preceded by 0xFF
        for off in find_all(rom, b'\xFFBIT'):
            if off > 0 and rom[off-1] == 0x00:
                bit_offsets.append(off - 1)

    if not bit_offsets:
        log("  ERROR: BIT header not found!")
        return None, []

    bit_offset = bit_offsets[0]
    log(f"  BIT Header at: 0x{bit_offset:06X}")
    log(hexdump(rom[bit_offset:bit_offset+16], bit_offset, prefix="    "))
    log()

    # BIT header: sig(4) + headerSize(u16) + ???
    # Standard format: after the 4-byte signature
    # Actually BIT starts at the 0x00 byte, signature is 0x00 0xFF 'B' 'I' 'T' (5 bytes)
    # But let's detect the actual format
    # The BIT header is: sig(2: 0x00FF) + "BIT\0" + version(u8) + headerSize(u8) + tokenSize(u8) + tokenCount(u8) + ...

    # Standard BIT layout (starting from bit_offset):
    # +0x00: 0x00
    # +0x01: 0xFF
    # +0x02: 'B'
    # +0x03: 'I'
    # +0x04: 'T'
    # +0x05: 0x00
    # +0x06: header checksum? or version
    # ...
    # Let's just look at the bytes

    raw = rom[bit_offset:bit_offset+32]
    log(f"  BIT raw header (32 bytes):")
    log(hexdump(raw, bit_offset, prefix="    "))
    log()

    # The actual BIT structure (from nouveau/envytools):
    # bytes 0-1: signature 0xB00F or 0xFF00 indicator
    # Actually for NVIDIA VBIOS:
    # The BIT starts at +0x02 after signature bytes
    # BIT header: signature(2) + bodySize(1) + headerVersion(1) + headerSize(1) + tokenSize(1) + tokenCount(1) + checksum(1)

    # Let's use offset 0x1B0 as known from previous analysis
    # BIT at 0x1B0:
    # 0x1B0: 0x00 0xFF 0x42 0x49 0x54 = \0\xFF BIT
    # After BIT signature (5 bytes + \0 terminator = 6 bytes)
    # Then: version(u8) + headerSize(u8) + tokenSize(u8) + tokenCount(u8)

    # Let's parse more carefully
    sig_end = bit_offset + 6  # past "BIT\0"
    if rom[bit_offset + 5] != 0:
        sig_end = bit_offset + 5

    log(f"  Parsing BIT token table starting after signature...")
    log()

    # From previous analysis we know 19 tokens starting at a specific offset
    # Let's scan for the tokens. In NVIDIA VBIOS the token entries are 6 bytes each:
    # tokenId(u8) + dataVersion(u8) + dataSize(u16) + dataOffset(u16)

    # The BIT token table typically starts right after the header
    # Header is typically 12 bytes: id(2) + sig(4) + ver(1) + hdr_sz(1) + tok_sz(1) + tok_cnt(1) + chk(1) + pad(1)

    # Let's try different header sizes
    tokens = []
    best_tokens = []
    best_hdr_end = 0

    for hdr_size in range(6, 20):
        token_start = bit_offset + hdr_size
        test_tokens = []
        for i in range(30):  # max 30 tokens
            t_off = token_start + i * 6
            if t_off + 6 > len(rom):
                break
            t_id = read_u8(rom, t_off)
            t_ver = read_u8(rom, t_off + 1)
            t_size = read_u16(rom, t_off + 2)
            t_data_off = read_u16(rom, t_off + 4)

            # Token 0x00 with size 0 and offset 0 = end marker
            if t_id == 0 and t_ver == 0 and t_size == 0 and t_data_off == 0:
                break

            # Sanity checks
            if t_data_off > 0 and t_data_off < len(rom) and t_size < 4096:
                test_tokens.append((t_id, t_ver, t_size, t_data_off))
            else:
                break

        if len(test_tokens) > len(best_tokens):
            best_tokens = test_tokens
            best_hdr_end = token_start

    tokens = best_tokens
    token_start = best_hdr_end

    log(f"  Token table starts at: 0x{token_start:06X}")
    log(f"  Number of tokens: {len(tokens)}")
    log()

    falcon_data_offset = None

    for idx, (t_id, t_ver, t_size, t_data_off) in enumerate(tokens):
        name = BIT_TOKEN_NAMES.get(t_id, f"UNKNOWN_0x{t_id:02X}")
        log(f"  Token #{idx:2d}: ID=0x{t_id:02X} ({name:20s}) Ver={t_ver} Size={t_size:4d} DataOffset=0x{t_data_off:04X}")

        # Show first bytes of token data
        if t_data_off > 0 and t_data_off + min(t_size, 32) <= len(rom):
            log(f"    Data preview:")
            log(hexdump(rom[t_data_off:t_data_off+min(t_size, 64)], t_data_off, prefix="      "))

        # Token 0x70 = Falcon Data
        if t_id == 0x70:
            log(f"    ** FALCON DATA TOKEN **")
            if t_size >= 4:
                falcon_data_offset = read_u32(rom, t_data_off)
                log(f"    ucodeTableOffset = 0x{falcon_data_offset:08X}")

        # Token 0x50 = PERF/PMU
        if t_id == 0x50:
            log(f"    ** PMU/PERF TOKEN **")
            if t_size >= 8:
                for j in range(0, min(t_size, 32), 4):
                    val = read_u32(rom, t_data_off + j)
                    log(f"    +{j:02X}: 0x{val:08X}")

        # Token 'I' (0x49) = INIT tables
        if t_id == 0x49:
            log(f"    ** INIT TOKEN **")
            if t_size >= 4:
                for j in range(0, min(t_size, 16), 2):
                    val = read_u16(rom, t_data_off + j)
                    log(f"    +{j:02X}: 0x{val:04X}")

        log()

    return falcon_data_offset, tokens


def analyze_falcon_ucode_table(rom, table_offset):
    """Parse the Falcon Ucode Lookup Table."""
    log("=" * 80)
    log("SECTION 3: FALCON UCODE LOOKUP TABLE")
    log("=" * 80)
    log()

    if table_offset is None or table_offset >= len(rom):
        log(f"  ERROR: Invalid table offset: 0x{table_offset:08X if table_offset else 0:08X}")
        return None

    log(f"  Table at offset: 0x{table_offset:06X}")
    log(f"  Raw header (16 bytes):")
    log(hexdump(rom[table_offset:table_offset+16], table_offset, prefix="    "))
    log()

    # Header: version(u8) + headerSize(u8) + entrySize(u8) + entryCount(u8) + descSize(u16)
    version = read_u8(rom, table_offset + 0)
    header_size = read_u8(rom, table_offset + 1)
    entry_size = read_u8(rom, table_offset + 2)
    entry_count = read_u8(rom, table_offset + 3)
    desc_size = read_u16(rom, table_offset + 4)

    log(f"  Header:")
    log(f"    Version:     {version}")
    log(f"    Header Size: {header_size} bytes")
    log(f"    Entry Size:  {entry_size} bytes")
    log(f"    Entry Count: {entry_count}")
    log(f"    Desc Size:   {desc_size} bytes (0x{desc_size:04X})")
    log()

    if entry_count == 0 or entry_count > 100:
        log(f"  WARNING: Suspicious entry count ({entry_count}), checking alternate parse...")
        # Try with desc_size as u8 instead of u16
        desc_size_alt = read_u8(rom, table_offset + 4)
        log(f"    Alternate descSize (u8): {desc_size_alt}")

    entries_start = table_offset + header_size
    log(f"  Entries start at: 0x{entries_start:06X}")
    log()

    fwsec_entry = None
    entries = []

    for i in range(entry_count):
        entry_off = entries_start + i * entry_size

        if entry_off + entry_size > len(rom):
            log(f"  Entry #{i}: OUT OF BOUNDS at 0x{entry_off:06X}")
            break

        if entry_size == 4:
            app_id = read_u16(rom, entry_off)
            data_offset = read_u16(rom, entry_off + 2)
        elif entry_size == 6:
            # PmuLookupEntryAda: appId(u16) + dataOffset(u32)
            app_id = read_u16(rom, entry_off)
            data_offset = read_u32(rom, entry_off + 2)
        elif entry_size == 8:
            app_id = read_u16(rom, entry_off)
            # Could be padding + offset or two u32s
            data_offset = read_u32(rom, entry_off + 4)
            extra = read_u16(rom, entry_off + 2)
            log(f"  Entry #{i}: raw={rom[entry_off:entry_off+8].hex()}")
        else:
            app_id = read_u16(rom, entry_off)
            data_offset = read_u32(rom, entry_off + 2) if entry_size >= 6 else 0

        app_name = APP_ID_NAMES.get(app_id, f"UNKNOWN_0x{app_id:04X}")
        entry_info = {
            "index": i,
            "app_id": app_id,
            "app_name": app_name,
            "data_offset": data_offset,
        }
        entries.append(entry_info)

        in_rom = "YES" if data_offset < len(rom) else "NO (out of range!)"
        log(f"  Entry #{i:2d}: AppID=0x{app_id:04X} ({app_name:12s}) DataOffset=0x{data_offset:08X} [InROM: {in_rom}]")

        # Raw bytes
        log(f"    Raw: {rom[entry_off:entry_off+entry_size].hex()}")

        if app_id == 0x85:
            fwsec_entry = entry_info
            log(f"    *** FWSEC ENTRY FOUND! ***")

        if app_id == 0x45:
            log(f"    *** FWSEC_DBG ENTRY FOUND! ***")

        log()

    log(f"  Total entries parsed: {len(entries)}")
    log()

    # Show the entire raw table
    total_table_size = header_size + entry_count * entry_size
    log(f"  Complete raw table ({total_table_size} bytes):")
    log(hexdump(rom[table_offset:table_offset + total_table_size], table_offset, prefix="    "))
    log()

    return fwsec_entry, entries


def analyze_fwsec_descriptor(rom, desc_offset):
    """Parse FalconUcodeDescV3Nvidia at the given offset."""
    log("=" * 80)
    log("SECTION 4: FWSEC DESCRIPTOR (FalconUcodeDescV3Nvidia)")
    log("=" * 80)
    log()

    if desc_offset is None or desc_offset >= len(rom):
        log(f"  ERROR: Invalid descriptor offset: 0x{desc_offset:08X if desc_offset else 0:08X}")
        return None

    log(f"  Descriptor at offset: 0x{desc_offset:06X}")
    log(f"  Raw data (64 bytes):")
    log(hexdump(rom[desc_offset:desc_offset+64], desc_offset, prefix="    "))
    log()

    # FalconUcodeDescV3Nvidia (44 bytes):
    # vDesc(u32) + storedSize(u32) + pkcDataOffset(u32) + interfaceOffset(u32) +
    # imemPhysBase(u32) + imemLoadSize(u32) + imemVirtBase(u32) +
    # dmemPhysBase(u32) + dmemLoadSize(u32) +
    # engineIdMask(u16) + ucodeId(u8) + signatureCount(u8) +
    # signatureVersions(u16) + reserved(u16)

    vDesc = read_u32(rom, desc_offset + 0)
    storedSize = read_u32(rom, desc_offset + 4)
    pkcDataOffset = read_u32(rom, desc_offset + 8)
    interfaceOffset = read_u32(rom, desc_offset + 12)
    imemPhysBase = read_u32(rom, desc_offset + 16)
    imemLoadSize = read_u32(rom, desc_offset + 20)
    imemVirtBase = read_u32(rom, desc_offset + 24)
    dmemPhysBase = read_u32(rom, desc_offset + 28)
    dmemLoadSize = read_u32(rom, desc_offset + 32)
    engineIdMask = read_u16(rom, desc_offset + 36)
    ucodeId = read_u8(rom, desc_offset + 38)
    signatureCount = read_u8(rom, desc_offset + 39)
    signatureVersions = read_u16(rom, desc_offset + 40)
    reserved = read_u16(rom, desc_offset + 42)

    log(f"  Parsed FalconUcodeDescV3Nvidia:")
    log(f"    vDesc:              0x{vDesc:08X}")
    log(f"    storedSize:         0x{storedSize:08X} ({storedSize} bytes / {storedSize/1024:.1f} KB)")
    log(f"    pkcDataOffset:      0x{pkcDataOffset:08X}")
    log(f"    interfaceOffset:    0x{interfaceOffset:08X}")
    log(f"    imemPhysBase:       0x{imemPhysBase:08X} (ROM offset of IMEM / code)")
    log(f"    imemLoadSize:       0x{imemLoadSize:08X} ({imemLoadSize} bytes / {imemLoadSize/1024:.1f} KB)")
    log(f"    imemVirtBase:       0x{imemVirtBase:08X} (virtual base address)")
    log(f"    dmemPhysBase:       0x{dmemPhysBase:08X} (ROM offset of DMEM / data)")
    log(f"    dmemLoadSize:       0x{dmemLoadSize:08X} ({dmemLoadSize} bytes / {dmemLoadSize/1024:.1f} KB)")
    log(f"    engineIdMask:       0x{engineIdMask:04X}")
    log(f"    ucodeId:            0x{ucodeId:02X} ({ucodeId})")
    log(f"    signatureCount:     {signatureCount}")
    log(f"    signatureVersions:  0x{signatureVersions:04X}")
    log(f"    reserved:           0x{reserved:04X}")
    log()

    # Engine ID interpretation
    engine_names = {0x01: "PMU", 0x02: "DISP", 0x04: "SEC2", 0x08: "NVDEC", 0x10: "GSP"}
    engines = []
    for bit in range(16):
        if engineIdMask & (1 << bit):
            engines.append(engine_names.get(1 << bit, f"BIT{bit}"))
    log(f"    Engine(s):          {', '.join(engines) if engines else 'NONE'}")
    log()

    # Validate offsets
    log(f"  Offset Validation:")
    log(f"    IMEM region: ROM 0x{imemPhysBase:06X} - 0x{imemPhysBase + imemLoadSize:06X}")
    log(f"    DMEM region: ROM 0x{dmemPhysBase:06X} - 0x{dmemPhysBase + dmemLoadSize:06X}")

    imem_in_rom = imemPhysBase + imemLoadSize <= len(rom)
    dmem_in_rom = dmemPhysBase + dmemLoadSize <= len(rom)
    log(f"    IMEM in ROM: {'YES' if imem_in_rom else 'NO - OUT OF BOUNDS!'}")
    log(f"    DMEM in ROM: {'YES' if dmem_in_rom else 'NO - OUT OF BOUNDS!'}")
    log()

    # Signatures
    sig_start = desc_offset + 44
    sig_size = 384  # RSA-3K = 384 bytes
    total_sig_size = signatureCount * sig_size

    log(f"  RSA-3K Signatures:")
    log(f"    Start: 0x{sig_start:06X}")
    log(f"    Count: {signatureCount}")
    log(f"    Each:  {sig_size} bytes")
    log(f"    Total: {total_sig_size} bytes")
    log()

    for i in range(min(signatureCount, 8)):  # Show max 8
        s_off = sig_start + i * sig_size
        if s_off + sig_size <= len(rom):
            log(f"    Signature #{i} at 0x{s_off:06X} (first 32 bytes):")
            log(hexdump(rom[s_off:s_off+32], s_off, prefix="      "))
            # Check if signature is all zeros
            is_zero = all(b == 0 for b in rom[s_off:s_off+sig_size])
            if is_zero:
                log(f"      (ALL ZEROS)")
            log()

    # Data after signatures (this should be the actual ucode)
    data_start = sig_start + total_sig_size
    log(f"  Ucode data starts at: 0x{data_start:06X}")
    log(f"  Expected storedSize bytes from descriptor start: 0x{desc_offset:06X} + 0x{storedSize:X} = 0x{desc_offset + storedSize:06X}")
    log()

    desc_info = {
        "vDesc": vDesc,
        "storedSize": storedSize,
        "pkcDataOffset": pkcDataOffset,
        "interfaceOffset": interfaceOffset,
        "imemPhysBase": imemPhysBase,
        "imemLoadSize": imemLoadSize,
        "imemVirtBase": imemVirtBase,
        "dmemPhysBase": dmemPhysBase,
        "dmemLoadSize": dmemLoadSize,
        "engineIdMask": engineIdMask,
        "ucodeId": ucodeId,
        "signatureCount": signatureCount,
        "signatureVersions": signatureVersions,
        "desc_offset": desc_offset,
    }

    # Show IMEM header (code start)
    if imem_in_rom:
        log(f"  IMEM (Code) Header at 0x{imemPhysBase:06X}:")
        log(hexdump(rom[imemPhysBase:imemPhysBase+64], imemPhysBase, prefix="    "))
        log()

    # Show DMEM header (data start)
    if dmem_in_rom:
        log(f"  DMEM (Data) Header at 0x{dmemPhysBase:06X}:")
        log(hexdump(rom[dmemPhysBase:dmemPhysBase+64], dmemPhysBase, prefix="    "))
        log()

    return desc_info


def analyze_dmemmapper(rom, desc_info):
    """Find and parse DMEMMAPPER in the FWSEC DMEM region."""
    log("=" * 80)
    log("SECTION 5: DMEMMAPPER (DMAP) ANALYSIS")
    log("=" * 80)
    log()

    if desc_info is None:
        log("  ERROR: No FWSEC descriptor info available")
        return None

    dmemPhysBase = desc_info["dmemPhysBase"]
    dmemLoadSize = desc_info["dmemLoadSize"]
    interfaceOffset = desc_info["interfaceOffset"]

    # Method 1: Direct calculation from interfaceOffset
    dmap_offset = dmemPhysBase + interfaceOffset
    log(f"  Method 1: DMAP at dmemPhysBase + interfaceOffset")
    log(f"    dmemPhysBase   = 0x{dmemPhysBase:08X}")
    log(f"    interfaceOffset = 0x{interfaceOffset:08X}")
    log(f"    Expected DMAP  = 0x{dmap_offset:08X}")
    log()

    dmap_found = False
    dmap_info = None

    if dmap_offset + 64 <= len(rom):
        sig = read_u32(rom, dmap_offset)
        log(f"    Signature at 0x{dmap_offset:06X}: 0x{sig:08X}", )
        if sig == 0x50414D44:  # "DMAP"
            log(f"    *** DMAP SIGNATURE FOUND! ***")
            dmap_found = True
        else:
            log(f"    Expected 0x50414D44 ('DMAP'), got 0x{sig:08X} ('{struct.pack('<I', sig).decode('ascii', errors='replace')}')")
            log(f"    Showing raw data:")
            log(hexdump(rom[dmap_offset:dmap_offset+64], dmap_offset, prefix="      "))
    else:
        log(f"    Offset 0x{dmap_offset:08X} is out of ROM bounds ({len(rom)} bytes)")
    log()

    # Method 2: Scan for DMAP signature in DMEM region
    log(f"  Method 2: Scanning DMEM region for DMAP signature (0x50414D44)")
    dmap_pattern = struct.pack("<I", 0x50414D44)

    if dmemPhysBase + dmemLoadSize <= len(rom):
        dmem_region = rom[dmemPhysBase:dmemPhysBase + dmemLoadSize]
        dmap_hits = find_all(dmem_region, dmap_pattern)
        if dmap_hits:
            for hit in dmap_hits:
                abs_off = dmemPhysBase + hit
                log(f"    DMAP found in DMEM at offset 0x{abs_off:06X} (DMEM+0x{hit:04X})")
                dmap_found = True
                dmap_offset = abs_off
        else:
            log(f"    No DMAP in DMEM region (0x{dmemPhysBase:06X} - 0x{dmemPhysBase + dmemLoadSize:06X})")
    else:
        log(f"    DMEM region extends beyond ROM")
    log()

    # Method 3: Global scan for DMAP
    log(f"  Method 3: Global scan for DMAP signature")
    global_dmap_hits = find_all(rom, dmap_pattern)
    if global_dmap_hits:
        for hit in global_dmap_hits:
            log(f"    DMAP at 0x{hit:06X}")
            if not dmap_found:
                dmap_offset = hit
                dmap_found = True
    else:
        log(f"    No DMAP signature found anywhere in ROM")
    log()

    # Also scan for "DMAP" as ASCII (big-endian: 0x444D4150)
    dmap_be_pattern = b'DMAP'
    be_hits = find_all(rom, dmap_be_pattern)
    if be_hits and be_hits != global_dmap_hits:
        log(f"  Method 3b: 'DMAP' as ASCII (big-endian)")
        for hit in be_hits:
            log(f"    'DMAP' at 0x{hit:06X}")
            if not dmap_found:
                dmap_offset = hit
                dmap_found = True
        log()

    # Parse DMAP if found
    if dmap_found and dmap_offset + 64 <= len(rom):
        log(f"  Parsing DMEMMAPPER at 0x{dmap_offset:06X}:")
        log(hexdump(rom[dmap_offset:dmap_offset+80], dmap_offset, prefix="    "))
        log()

        # DMEMMAPPER structure:
        # signature(u32) + version(u16) + size(u16) + cmdBufOffset(u32) + cmdBufSize(u32) +
        # dataBufOffset(u32) + dataBufSize(u32) + initCmd(u32) + reserved[8](u32)
        dmap_sig = read_u32(rom, dmap_offset + 0)
        dmap_version = read_u16(rom, dmap_offset + 4)
        dmap_size = read_u16(rom, dmap_offset + 6)
        cmdBufOffset = read_u32(rom, dmap_offset + 8)
        cmdBufSize = read_u32(rom, dmap_offset + 12)
        dataBufOffset = read_u32(rom, dmap_offset + 16)
        dataBufSize = read_u32(rom, dmap_offset + 20)
        initCmd = read_u32(rom, dmap_offset + 24)

        log(f"    signature:    0x{dmap_sig:08X} ('{struct.pack('<I', dmap_sig).decode('ascii', errors='replace')}')")
        log(f"    version:      {dmap_version} (0x{dmap_version:04X})")
        log(f"    size:         {dmap_size} bytes")
        log(f"    cmdBufOffset: 0x{cmdBufOffset:08X}")
        log(f"    cmdBufSize:   0x{cmdBufSize:08X} ({cmdBufSize} bytes)")
        log(f"    dataBufOffset:0x{dataBufOffset:08X}")
        log(f"    dataBufSize:  0x{dataBufSize:08X} ({dataBufSize} bytes)")
        log(f"    initCmd:      0x{initCmd:08X}")
        log()

        # Known initCmd values: 0x15 = FRTS, 0x19 = SB (Secure Boot)
        cmd_names = {0x15: "FRTS (Setup WPR2)", 0x19: "SB (Secure Boot)"}
        log(f"    initCmd interpreted: {cmd_names.get(initCmd, 'UNKNOWN')}")
        log()

        # Reserved fields
        log(f"    Reserved fields:")
        for i in range(8):
            rval = read_u32(rom, dmap_offset + 28 + i * 4)
            if rval != 0:
                log(f"      reserved[{i}]: 0x{rval:08X}")
        log()

        # CMD buffer contents
        cmd_buf_abs = dmemPhysBase + cmdBufOffset if cmdBufOffset < 0x100000 else cmdBufOffset
        if cmd_buf_abs + cmdBufSize <= len(rom) and cmdBufSize > 0:
            log(f"    CMD Buffer at 0x{cmd_buf_abs:06X} ({cmdBufSize} bytes):")
            log(hexdump(rom[cmd_buf_abs:cmd_buf_abs + min(cmdBufSize, 128)], cmd_buf_abs, prefix="      "))
            log()

        # DATA buffer contents
        data_buf_abs = dmemPhysBase + dataBufOffset if dataBufOffset < 0x100000 else dataBufOffset
        if data_buf_abs + dataBufSize <= len(rom) and dataBufSize > 0:
            log(f"    DATA Buffer at 0x{data_buf_abs:06X} ({dataBufSize} bytes):")
            log(hexdump(rom[data_buf_abs:data_buf_abs + min(dataBufSize, 128)], data_buf_abs, prefix="      "))
            log()

        dmap_info = {
            "offset": dmap_offset,
            "version": dmap_version,
            "size": dmap_size,
            "cmdBufOffset": cmdBufOffset,
            "cmdBufSize": cmdBufSize,
            "dataBufOffset": dataBufOffset,
            "dataBufSize": dataBufSize,
            "initCmd": initCmd,
        }
    elif not dmap_found:
        log("  DMEMMAPPER NOT FOUND - trying alternate approaches...")
        log()

        # Look for any 4-byte pattern that could be a version of DMAP
        # Try scanning for the command values (0x15, 0x19)
        log("  Scanning for FRTS command (0x15) / SB command (0x19) in DMEM:")
        if dmemPhysBase + dmemLoadSize <= len(rom):
            for needle_val, needle_name in [(0x15, "FRTS"), (0x19, "SB")]:
                needle = struct.pack("<I", needle_val)
                hits = find_all(rom[dmemPhysBase:dmemPhysBase + dmemLoadSize], needle)
                for h in hits[:5]:
                    abs_h = dmemPhysBase + h
                    log(f"    0x{needle_val:02X} ({needle_name}) at DMEM+0x{h:04X} (ROM 0x{abs_h:06X})")
                    log(hexdump(rom[abs_h-16:abs_h+48], abs_h-16, prefix="      "))
                    log()

    return dmap_info


def analyze_region_between_efi_and_falcon(rom, efi_end=0x24A00, falcon_table=0x80DE8):
    """Analyze the data between EFI image end and falcon table."""
    log("=" * 80)
    log("SECTION 6: REGION ANALYSIS (0x{:06X} - 0x{:06X})".format(efi_end, falcon_table))
    log("=" * 80)
    log()

    region_size = falcon_table - efi_end
    log(f"  Region size: {region_size} bytes ({region_size/1024:.1f} KB)")
    log()

    # Check for "NV" signature at start
    if efi_end + 2 <= len(rom):
        nv_bytes = rom[efi_end:efi_end+2]
        log(f"  First 2 bytes at 0x{efi_end:06X}: 0x{nv_bytes[0]:02X} 0x{nv_bytes[1]:02X} = '{chr(nv_bytes[0]) if 32<=nv_bytes[0]<127 else '?'}{chr(nv_bytes[1]) if 32<=nv_bytes[1]<127 else '?'}'")
        if nv_bytes == b'\x56\x4E' or nv_bytes == b'\x4E\x56':
            log(f"    ** 'NV' or 'VN' SIGNATURE FOUND! **")
    log()

    # Show first 128 bytes
    log(f"  Header of region:")
    log(hexdump(rom[efi_end:efi_end+128], efi_end, prefix="    "))
    log()

    # Scan for known NVIDIA signatures / magic numbers
    log(f"  Scanning for known signatures in region:")

    signatures_to_find = [
        (b'\xAA\x55', "PCI ROM (AA55)"),
        (b'PCIR', "PCI Data Structure"),
        (b'NPDE', "NVIDIA PCI Data Extension"),
        (b'\x00\xFFBIT', "BIT Header"),
        (b'NV', "NV Signature"),
        (b'\x56\x4E', "VN (reversed NV)"),
        (b'DMAP', "DMEMMAPPER"),
        (b'NVGI', "NVIDIA GPU Init"),
        (b'NVMM', "NVIDIA Memory Manager"),
        (b'\x44\x45\x56\x49', "DEVI (devinit?)"),
        (struct.pack("<I", 0x10DE), "NVIDIA Vendor ID (LE)"),
        (struct.pack(">I", 0x10DE), "NVIDIA Vendor ID (BE)"),
        (struct.pack("<H", 0x10DE), "NVIDIA Vendor ID (u16 LE)"),
        (struct.pack("<H", 0x2684), "RTX 4090 Device ID"),
    ]

    region = rom[efi_end:falcon_table]
    for sig, name in signatures_to_find:
        hits = find_all(region, sig)
        if hits:
            for h in hits[:3]:
                abs_h = efi_end + h
                log(f"    {name} at 0x{abs_h:06X} (region+0x{h:04X})")

    log()

    # Look for NVIDIA devinit script tables
    # Devinit scripts start with opcodes. Common: 0x49 (INIT_REG), 0x32 (INIT32)
    log(f"  Scanning for potential devinit script opcodes:")

    # Look for init script header patterns
    # NVIDIA init scripts often start with 0x00000000 pointer entries
    # Then followed by opcode streams

    # Check for structured data (potential tables)
    # Look for patterns of u32 offsets that point within the region
    table_candidates = []
    for off in range(0, min(region_size - 16, 4096), 4):
        val = read_u32(rom, efi_end + off)
        if efi_end <= val < falcon_table and val != 0:
            table_candidates.append((off, val))

    if table_candidates:
        log(f"  Potential internal pointers (first 20):")
        for off, val in table_candidates[:20]:
            log(f"    +0x{off:04X}: -> 0x{val:06X}")
        log()

    # Detect large blocks of 0xFF (unused regions)
    log(f"  Region usage map (512-byte blocks):")
    block_size = 512
    for blk in range(0, region_size, block_size):
        chunk = region[blk:blk+block_size]
        zeros = sum(1 for b in chunk if b == 0x00)
        ffs = sum(1 for b in chunk if b == 0xFF)
        total = len(chunk)

        if ffs > total * 0.9:
            status = "EMPTY (0xFF)"
        elif zeros > total * 0.9:
            status = "EMPTY (0x00)"
        elif ffs > total * 0.5:
            status = "MOSTLY_EMPTY"
        else:
            # Try to identify content
            has_ascii = sum(1 for b in chunk if 32 <= b < 127) > total * 0.3
            status = "DATA" + (" (has ASCII)" if has_ascii else "")

        abs_addr = efi_end + blk
        log(f"    0x{abs_addr:06X}-0x{abs_addr+block_size:06X}: {status}")

    log()

    # Look for NVIDIA devinit table headers
    # These typically have: version(u8) + headerSize(u8) + entrySize(u8) + entryCount(u8)
    log(f"  Scanning for table-like structures (version/headerSize/entrySize/entryCount):")
    for off in range(0, min(region_size - 16, 65536)):
        ver = read_u8(rom, efi_end + off)
        hdr_sz = read_u8(rom, efi_end + off + 1)
        ent_sz = read_u8(rom, efi_end + off + 2)
        ent_cnt = read_u8(rom, efi_end + off + 3)

        # Reasonable table: version 1-3, header 4-32, entry 2-64, count 1-100
        if (1 <= ver <= 5 and 4 <= hdr_sz <= 32 and 2 <= ent_sz <= 64 and 2 <= ent_cnt <= 100):
            total_sz = hdr_sz + ent_sz * ent_cnt
            if total_sz < 4096:
                abs_off = efi_end + off
                log(f"    0x{abs_off:06X}: ver={ver} hdrSz={hdr_sz} entrySz={ent_sz} entryCnt={ent_cnt} totalSz={total_sz}")
                log(hexdump(rom[abs_off:abs_off+min(hdr_sz + ent_sz * min(ent_cnt, 4), 64)], abs_off, prefix="      "))
                log()

    log()


def scan_fwsec_code_type(rom, images):
    """Scan for FWSEC image (code type 0xE0) in PCI ROM images."""
    log("=" * 80)
    log("SECTION 7: FWSEC CODE TYPE 0xE0 SCAN")
    log("=" * 80)
    log()

    fwsec_images = [img for img in images if img["code_type"] == 0xE0]
    if fwsec_images:
        for img in fwsec_images:
            log(f"  FWSEC image found at 0x{img['offset']:06X}!")
            log(f"    Size: {img['size']} bytes")
    else:
        log(f"  No code type 0xE0 images found in PCI ROM structure.")
        log()

    # Also scan for 0xAA55 beyond the tracked images
    log(f"  Extended scan for AA55 signatures after known images:")
    offset = 0
    while offset < len(rom) - 2:
        if read_u16(rom, offset) == 0xAA55:
            # Check for PCIR
            if offset + 0x1A < len(rom):
                pcir_off = read_u16(rom, offset + 0x18)
                pcir_abs = offset + pcir_off
                if pcir_abs + 24 < len(rom) and rom[pcir_abs:pcir_abs+4] == b'PCIR':
                    code_type = read_u8(rom, pcir_abs + 20)
                    image_len = read_u16(rom, pcir_abs + 16) * 512
                    ct_name = {0x00: "x86/AT", 0x03: "EFI", 0xE0: "FWSEC"}.get(code_type, f"0x{code_type:02X}")
                    log(f"    AA55 at 0x{offset:06X}: CodeType={ct_name} Size={image_len}")
                    if code_type == 0xE0:
                        log(f"      *** FWSEC IMAGE (Code Type 0xE0) ***")
                        log(hexdump(rom[offset:offset+128], offset, prefix="        "))
                else:
                    log(f"    AA55 at 0x{offset:06X}: No valid PCIR")
            offset += 0x200
        else:
            offset += 0x200
    log()


def analyze_pmu_table(rom, tokens):
    """Re-analyze PMU/PERF table with full ROM."""
    log("=" * 80)
    log("SECTION 8: PMU TABLE RE-ANALYSIS (Full ROM)")
    log("=" * 80)
    log()

    pmu_token = None
    for t_id, t_ver, t_size, t_data_off in tokens:
        if t_id == 0x50:
            pmu_token = (t_id, t_ver, t_size, t_data_off)
            break

    if pmu_token is None:
        log("  No PMU token (0x50) found in BIT")
        log()
        # Try finding falcon-related tokens
        for t_id, t_ver, t_size, t_data_off in tokens:
            if t_id in [0x46, 0x70]:
                log(f"  Found Falcon-related token 0x{t_id:02X}: ver={t_ver} size={t_size} offset=0x{t_data_off:04X}")
        return

    t_id, t_ver, t_size, t_data_off = pmu_token
    log(f"  PMU Token: ID=0x{t_id:02X} Ver={t_ver} Size={t_size} Offset=0x{t_data_off:04X}")
    log()

    log(f"  Full PMU data ({t_size} bytes):")
    log(hexdump(rom[t_data_off:t_data_off + min(t_size, 256)], t_data_off, prefix="    "))
    log()

    # Parse as table of offsets/pointers
    log(f"  Interpreting as u32 table:")
    for i in range(0, min(t_size, 128), 4):
        if t_data_off + i + 4 <= len(rom):
            val = read_u32(rom, t_data_off + i)
            in_rom = "-> IN ROM" if 0 < val < len(rom) else ""
            log(f"    +0x{i:02X}: 0x{val:08X} {in_rom}")
            if 0 < val < len(rom):
                # Show what's at the pointed location
                log(f"           -> {rom[val:val+16].hex()}")
    log()

    # Parse as u16 table
    log(f"  Interpreting as u16 table:")
    for i in range(0, min(t_size, 64), 2):
        if t_data_off + i + 2 <= len(rom):
            val = read_u16(rom, t_data_off + i)
            in_rom = "-> IN ROM" if 0 < val < len(rom) else ""
            log(f"    +0x{i:02X}: 0x{val:04X} {in_rom}")
    log()


def analyze_all_falcon_descriptors(rom, entries):
    """Parse descriptors for ALL falcon ucode entries, not just FWSEC."""
    log("=" * 80)
    log("SECTION 9: ALL FALCON UCODE DESCRIPTORS")
    log("=" * 80)
    log()

    for entry in entries:
        app_id = entry["app_id"]
        app_name = entry["app_name"]
        data_offset = entry["data_offset"]

        log(f"  --- AppID 0x{app_id:04X} ({app_name}) at 0x{data_offset:08X} ---")

        if data_offset >= len(rom) or data_offset + 44 > len(rom):
            log(f"    OUT OF ROM BOUNDS (ROM size: {len(rom)})")
            log()
            continue

        log(f"    Raw data (64 bytes):")
        log(hexdump(rom[data_offset:data_offset+64], data_offset, prefix="      "))
        log()

        vDesc = read_u32(rom, data_offset + 0)
        storedSize = read_u32(rom, data_offset + 4)
        pkcDataOffset = read_u32(rom, data_offset + 8)
        interfaceOffset = read_u32(rom, data_offset + 12)
        imemPhysBase = read_u32(rom, data_offset + 16)
        imemLoadSize = read_u32(rom, data_offset + 20)
        imemVirtBase = read_u32(rom, data_offset + 24)
        dmemPhysBase = read_u32(rom, data_offset + 28)
        dmemLoadSize = read_u32(rom, data_offset + 32)
        engineIdMask = read_u16(rom, data_offset + 36)
        ucodeId = read_u8(rom, data_offset + 38)
        signatureCount = read_u8(rom, data_offset + 39)
        signatureVersions = read_u16(rom, data_offset + 40)

        log(f"    vDesc:           0x{vDesc:08X}")
        log(f"    storedSize:      0x{storedSize:08X} ({storedSize} bytes)")
        log(f"    pkcDataOffset:   0x{pkcDataOffset:08X}")
        log(f"    interfaceOffset: 0x{interfaceOffset:08X}")
        log(f"    imemPhysBase:    0x{imemPhysBase:08X}")
        log(f"    imemLoadSize:    0x{imemLoadSize:08X} ({imemLoadSize} bytes)")
        log(f"    imemVirtBase:    0x{imemVirtBase:08X}")
        log(f"    dmemPhysBase:    0x{dmemPhysBase:08X}")
        log(f"    dmemLoadSize:    0x{dmemLoadSize:08X} ({dmemLoadSize} bytes)")
        log(f"    engineIdMask:    0x{engineIdMask:04X}")
        log(f"    ucodeId:         0x{ucodeId:02X}")
        log(f"    signatureCount:  {signatureCount}")
        log(f"    signatureVer:    0x{signatureVersions:04X}")

        imem_end = imemPhysBase + imemLoadSize
        dmem_end = dmemPhysBase + dmemLoadSize
        log(f"    IMEM: 0x{imemPhysBase:06X} - 0x{imem_end:06X} ({'IN ROM' if imem_end <= len(rom) else 'OUT OF BOUNDS'})")
        log(f"    DMEM: 0x{dmemPhysBase:06X} - 0x{dmem_end:06X} ({'IN ROM' if dmem_end <= len(rom) else 'OUT OF BOUNDS'})")
        log()


def analyze_rom_mirrors_and_tail(rom):
    """Analyze the mirror region and ROM tail."""
    log("=" * 80)
    log("SECTION 10: ROM TAIL AND MIRROR ANALYSIS")
    log("=" * 80)
    log()

    rom_size = len(rom)
    log(f"  ROM size: {rom_size} bytes (0x{rom_size:X})")
    log()

    # Check for mirror at 0xE0000
    mirror_offset = 0xE0000
    if mirror_offset < rom_size:
        log(f"  Mirror region at 0x{mirror_offset:06X}:")
        log(hexdump(rom[mirror_offset:mirror_offset+64], mirror_offset, prefix="    "))
        log()

        # Compare with start of ROM
        if rom[mirror_offset:mirror_offset+2] == b'\x55\xAA' or rom[mirror_offset:mirror_offset+2] == rom[0:2]:
            log(f"    This appears to be a ROM mirror (matches ROM start)")
        else:
            log(f"    Does not match ROM start - may be different data")
        log()

    # Check last few KB for any interesting data
    tail_start = max(0, rom_size - 4096)
    log(f"  Last 4KB of ROM (0x{tail_start:06X} - 0x{rom_size:06X}):")
    # Check if it's all FF
    tail = rom[tail_start:]
    ff_count = sum(1 for b in tail if b == 0xFF)
    zero_count = sum(1 for b in tail if b == 0x00)
    log(f"    0xFF bytes: {ff_count}/{len(tail)} ({ff_count*100/len(tail):.1f}%)")
    log(f"    0x00 bytes: {zero_count}/{len(tail)} ({zero_count*100/len(tail):.1f}%)")

    if ff_count < len(tail) * 0.8:
        log(f"    Non-empty data found in ROM tail:")
        log(hexdump(rom[tail_start:tail_start+128], tail_start, prefix="      "))
    log()

    # Check for checksum byte at very end
    log(f"  Last 16 bytes:")
    log(hexdump(rom[rom_size-16:], rom_size-16, prefix="    "))
    log()

    # Compute whole-ROM checksum (should be 0 for valid VBIOS)
    checksum = sum(rom) & 0xFF
    log(f"  Whole ROM checksum (mod 256): 0x{checksum:02X} ({'VALID (0)' if checksum == 0 else 'INVALID'})")
    log()


def generate_driver_summary(desc_info, dmap_info, entries):
    """Generate a summary for driver implementation."""
    log("=" * 80)
    log("SECTION 11: DRIVER IMPLEMENTATION SUMMARY")
    log("=" * 80)
    log()

    log("  FWSEC Extraction and Loading Steps for NVDAAL Driver:")
    log()

    if desc_info:
        log(f"  1. FWSEC Location in VBIOS:")
        log(f"     - Descriptor at ROM offset:  0x{desc_info['desc_offset']:06X}")
        log(f"     - IMEM (code) at ROM offset: 0x{desc_info['imemPhysBase']:06X}")
        log(f"     - IMEM size:                 {desc_info['imemLoadSize']} bytes ({desc_info['imemLoadSize']/1024:.1f} KB)")
        log(f"     - DMEM (data) at ROM offset: 0x{desc_info['dmemPhysBase']:06X}")
        log(f"     - DMEM size:                 {desc_info['dmemLoadSize']} bytes ({desc_info['dmemLoadSize']/1024:.1f} KB)")
        log(f"     - Total stored size:         {desc_info['storedSize']} bytes ({desc_info['storedSize']/1024:.1f} KB)")
        log(f"     - Signatures:                {desc_info['signatureCount']} x 384 bytes RSA-3K")
        log()

        log(f"  2. Extraction Path:")
        log(f"     a. Read VBIOS via _ROM ACPI method or BAR0 PROM register (0x300000)")
        log(f"     b. Find BIT header (scan for 0x00FF4249540x ('\\0\\xFFBIT'))")
        log(f"     c. Parse BIT token 0x70 to get Falcon Ucode Table offset")
        log(f"     d. Parse Falcon table, find AppID 0x0085 (FWSEC)")
        log(f"     e. Read FalconUcodeDescV3Nvidia at the entry's dataOffset")
        log(f"     f. Extract IMEM and DMEM regions from VBIOS ROM")
        log()

        log(f"  3. Loading FWSEC onto GSP Falcon (MMIO 0x110000):")
        log(f"     a. Set FALCON_DMACTL to enable DMA")
        log(f"     b. Load IMEM via Falcon IMEMC/IMEMD ports")
        log(f"        - Base: NV_PGSP_FALCON_IMEMC(0) = 0x110180")
        log(f"        - Data: NV_PGSP_FALCON_IMEMD(0) = 0x110184")
        log(f"     c. Load DMEM via Falcon DMEMC/DMEMD ports")
        log(f"        - Base: NV_PGSP_FALCON_DMEMC(0) = 0x1101C0")
        log(f"        - Data: NV_PGSP_FALCON_DMEMD(0) = 0x1101C4")
        log(f"     d. Set boot vector to imemVirtBase: 0x{desc_info['imemVirtBase']:08X}")
        log(f"        - NV_PGSP_FALCON_BOOTVEC = 0x110104")
        log(f"     e. Start Falcon execution")
        log(f"        - NV_PGSP_FALCON_CPUCTL = 0x110100, write START bit")
        log()

    if dmap_info:
        log(f"  4. DMEMMAPPER Configuration:")
        log(f"     - Patch DMEM at interfaceOffset with DMAP command")
        log(f"     - initCmd = 0x{dmap_info['initCmd']:02X} ({['FRTS (WPR2 setup)', 'SB (Secure Boot)'][dmap_info['initCmd'] == 0x19] if dmap_info['initCmd'] in [0x15, 0x19] else 'UNKNOWN'})")
        log(f"     - CMD buffer offset: 0x{dmap_info['cmdBufOffset']:X}")
        log(f"     - CMD buffer size: {dmap_info['cmdBufSize']} bytes")
        log(f"     - DATA buffer offset: 0x{dmap_info['dataBufOffset']:X}")
        log(f"     - DATA buffer size: {dmap_info['dataBufSize']} bytes")
        log()

    log(f"  5. FWSEC-FRTS Boot Flow:")
    log(f"     a. FWSEC runs on GSP Falcon in Heavy-Secure (HS) mode")
    log(f"     b. FWSEC-FRTS sets up WPR2 (Write Protected Region 2)")
    log(f"     c. WPR2 status registers:")
    log(f"        - NV_PFB_PRI_MMU_WPR2_ADDR_LO = 0x1FA824")
    log(f"        - NV_PFB_PRI_MMU_WPR2_ADDR_HI = 0x1FA828")
    log(f"     d. FRTS status: reg 0x001438 (upper 16 bits = error code)")
    log(f"     e. After FRTS: proceed to SEC2 booter load/unload")
    log()

    log(f"  6. Full Boot Sequence:")
    log(f"     a. Extract FWSEC from VBIOS -> Load FWSEC-FRTS -> Start GSP Falcon")
    log(f"     b. FWSEC-FRTS configures WPR2")
    log(f"     c. Load SEC2 booter_load onto SEC2 Falcon (0x840000)")
    log(f"     d. SEC2 booter_load loads GSP-RM firmware into WPR2")
    log(f"     e. Load SEC2 booter_unload (for cleanup)")
    log(f"     f. Start GSP-RM via RPC")
    log()

    # Register reference
    log(f"  7. Key Register Reference:")
    log(f"     GSP Falcon base:      0x110000")
    log(f"     SEC2 Falcon base:     0x840000")
    log(f"     FALCON_CPUCTL:        base+0x100")
    log(f"     FALCON_BOOTVEC:       base+0x104")
    log(f"     FALCON_HWCFG:         base+0x108")
    log(f"     FALCON_DMACTL:        base+0x10C")
    log(f"     FALCON_IMEMC(i):      base+0x180+i*16")
    log(f"     FALCON_IMEMD(i):      base+0x184+i*16")
    log(f"     FALCON_DMEMC(i):      base+0x1C0+i*16")
    log(f"     FALCON_DMEMD(i):      base+0x1C4+i*16")
    log(f"     FALCON_OS:            base+0x080")
    log(f"     NV_PROM (VBIOS read): 0x300000")
    log(f"     FRTS status:          0x001438")
    log(f"     WPR2 LO:              0x1FA824")
    log(f"     WPR2 HI:              0x1FA828")
    log()


def main():
    log(f"NVIDIA RTX 4090 (AD102) Full VBIOS FWSEC Analysis")
    log(f"Generated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    log(f"ROM: {ROM_PATH}")
    log()

    with open(ROM_PATH, "rb") as f:
        rom = f.read()

    log(f"ROM size: {len(rom)} bytes ({len(rom)/1024:.1f} KB)")
    log()

    # Section 1: PCI ROM images
    images = analyze_pci_rom_headers(rom)

    # Section 2: BIT header and tokens
    falcon_data_offset, tokens = analyze_bit_header(rom)

    # Section 3: Falcon Ucode Lookup Table
    fwsec_entry = None
    all_entries = []
    if falcon_data_offset is not None:
        result = analyze_falcon_ucode_table(rom, falcon_data_offset)
        if result:
            fwsec_entry, all_entries = result

    # Section 4: FWSEC Descriptor
    fwsec_desc = None
    if fwsec_entry is not None:
        fwsec_desc = analyze_fwsec_descriptor(rom, fwsec_entry["data_offset"])

    # Section 5: DMEMMAPPER
    dmap_info = analyze_dmemmapper(rom, fwsec_desc)

    # Section 6: Region between EFI and Falcon table
    analyze_region_between_efi_and_falcon(rom)

    # Section 7: FWSEC code type 0xE0 scan
    scan_fwsec_code_type(rom, images)

    # Section 8: PMU table re-analysis
    analyze_pmu_table(rom, tokens)

    # Section 9: All Falcon ucode descriptors
    if all_entries:
        analyze_all_falcon_descriptors(rom, all_entries)

    # Section 10: ROM tail and mirrors
    analyze_rom_mirrors_and_tail(rom)

    # Section 11: Driver implementation summary
    generate_driver_summary(fwsec_desc, dmap_info, all_entries)

    # Save output
    output = "\n".join(out_lines)
    with open(OUTPUT_PATH, "w") as f:
        f.write(output)

    print(f"\n{'='*80}")
    print(f"Analysis saved to: {OUTPUT_PATH}")
    print(f"Total lines: {len(out_lines)}")


if __name__ == "__main__":
    main()
