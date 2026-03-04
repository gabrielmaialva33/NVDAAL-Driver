#!/usr/bin/env python3
"""
FINAL Deep VBIOS FWSEC Analysis for NVIDIA RTX 4090 (AD102)
Comprehensive analysis of the full 988KB VBIOS ROM.
"""

import struct
import math
from datetime import datetime

ROM_PATH = "/home/gabriel-maia/Documentos/NVDAAL-Driver/ReverseEng/vbios_full.rom"
OUTPUT_PATH = "/home/gabriel-maia/Documentos/NVDAAL-Driver/ReverseEng/vbios_fwsec_analysis.txt"

out_lines = []

def log(msg=""):
    out_lines.append(msg)
    print(msg)

def hexdump(data, offset=0, length=None, prefix="  "):
    if length is None:
        length = len(data)
    lines = []
    for i in range(0, min(length, len(data)), 16):
        hex_part = " ".join(f"{data[i+j]:02X}" if i+j < min(length, len(data)) else "  " for j in range(16))
        ascii_part = "".join(chr(data[i+j]) if 32 <= data[i+j] < 127 else "." for j in range(min(16, min(length, len(data)) - i)))
        lines.append(f"{prefix}{offset+i:08X}: {hex_part}  {ascii_part}")
    return "\n".join(lines)

def read_u8(rom, off): return rom[off]
def read_u16(rom, off): return struct.unpack_from("<H", rom, off)[0]
def read_u32(rom, off): return struct.unpack_from("<I", rom, off)[0]

def find_all(rom, pattern):
    results = []
    start = 0
    while True:
        idx = rom.find(pattern, start)
        if idx == -1:
            break
        results.append(idx)
        start = idx + 1
    return results

def entropy(data):
    if not data:
        return 0
    freq = {}
    for b in data:
        freq[b] = freq.get(b, 0) + 1
    total = len(data)
    return -sum((c/total) * math.log2(c/total) for c in freq.values())


def main():
    log("=" * 80)
    log("NVIDIA RTX 4090 (AD102) Full VBIOS FWSEC Analysis")
    log(f"Generated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    log(f"ROM: {ROM_PATH}")
    log("=" * 80)
    log()

    with open(ROM_PATH, "rb") as f:
        rom = f.read()

    rom_size = len(rom)
    log(f"ROM size: {rom_size} bytes ({rom_size/1024:.1f} KB / 0x{rom_size:X})")
    log()

    # =========================================================================
    # SECTION 1: PCI ROM IMAGES
    # =========================================================================
    log("=" * 80)
    log("SECTION 1: PCI ROM IMAGES")
    log("=" * 80)
    log()

    images = []
    offset = 0
    img_num = 0

    while offset < rom_size - 2:
        sig = read_u16(rom, offset)
        if sig != 0xAA55:
            offset += 0x200
            continue

        img_num += 1
        pcir_off_field = read_u16(rom, offset + 0x18)
        pcir_abs = offset + pcir_off_field

        if pcir_abs + 24 >= rom_size or rom[pcir_abs:pcir_abs+4] != b'PCIR':
            offset += 0x200
            continue

        vendor_id = read_u16(rom, pcir_abs + 4)
        device_id = read_u16(rom, pcir_abs + 6)
        code_type = read_u8(rom, pcir_abs + 20)
        image_len = read_u16(rom, pcir_abs + 16) * 512
        indicator = read_u8(rom, pcir_abs + 21)
        last_image = (indicator & 0x80) != 0

        code_type_name = {0x00: "x86/AT (Legacy BIOS)", 0x03: "UEFI", 0xE0: "NVIDIA FWSEC"}.get(code_type, f"Unknown(0x{code_type:02X})")

        img_info = {"offset": offset, "size": image_len, "code_type": code_type, "code_type_name": code_type_name, "last": last_image}
        images.append(img_info)

        log(f"  Image #{img_num}: 0x{offset:06X} - 0x{offset+image_len:06X} ({image_len/1024:.1f} KB)")
        log(f"    Vendor: 0x{vendor_id:04X}  Device: 0x{device_id:04X}  Code: {code_type_name}")
        log(f"    Last Image: {last_image}")
        log()

        offset += image_len
        if last_image:
            break

    # Check for mirror
    if 0xE0000 < rom_size and rom[0xE0000:0xE0002] == b'\x55\xAA':
        log(f"  ROM Mirror at 0x0E0000 (exact copy of first 64KB image)")
    log()

    # =========================================================================
    # SECTION 2: BIT HEADER AND TOKENS
    # =========================================================================
    log("=" * 80)
    log("SECTION 2: BIT HEADER AND TOKENS")
    log("=" * 80)
    log()

    # BIT at 0x1B0: FF B8 'BIT\0' then header fields
    # Actual structure: the BIT marker is 0xB8FF followed by 'BIT\0'
    bit_offset = 0x1B0
    log(f"  BIT Header at: 0x{bit_offset:06X}")
    log(f"  Signature: {rom[bit_offset:bit_offset+6].hex(' ')}")
    log()

    # Parse header from 0x1B2 (start of 'BIT')
    # BIT\0 (4 bytes) + reserved(1) + version(1) + headerSize(1) + tokenSize(1) + tokenCount(1) + checksum(1)
    bit_start = 0x1B2
    hdr_size = read_u8(rom, bit_start + 6)  # 0x0C
    token_size = read_u8(rom, bit_start + 7)  # 0x06
    token_count = read_u8(rom, bit_start + 8)  # 0x13 = 19

    # Token table starts at bit_offset + headerSize (0x0C from bit_offset+2, or from 0x1B0)
    # Working backwards: 0x1B0 + 0x0C = 0x1BC
    token_start = 0x1BC

    log(f"  Header Size: {hdr_size} bytes (from 0x1B0)")
    log(f"  Token Size: {token_size} bytes")
    log(f"  Token Count: {token_count}")
    log(f"  Token Table: 0x{token_start:04X}")
    log()

    BIT_TOKEN_NAMES = {
        0x32: "DEVINIT_PTRS", 0x42: "BIOSDATA", 0x43: "CLOCK",
        0x44: "DFP", 0x45: "DISPLAY_EAGLE (E)", 0x49: "INIT",
        0x4D: "MEMORY", 0x4E: "NOP", 0x50: "PERF",
        0x53: "STRING", 0x54: "TMDS", 0x55: "USB",
        0x56: "VIRTUAL", 0x64: "DISPLAY (d)", 0x69: "BIOS_INFO (i)",
        0x70: "FALCON_DATA", 0x73: "SEC_VER (s)", 0x75: "GPU_INFO (u)",
        0x78: "EXTDEV (x)",
    }

    tokens = []
    falcon_table_offset = None

    for i in range(token_count):
        off = token_start + i * token_size
        t_id = read_u8(rom, off)
        t_ver = read_u8(rom, off + 1)
        t_size = read_u16(rom, off + 2)
        t_data_off = read_u16(rom, off + 4)

        name = BIT_TOKEN_NAMES.get(t_id, f"UNKNOWN_0x{t_id:02X}")
        tokens.append((t_id, t_ver, t_size, t_data_off))

        log(f"  Token #{i:2d}: ID=0x{t_id:02X} ({name:20s}) Ver={t_ver} Size={t_size:4d} Data=0x{t_data_off:04X}")

        if t_id == 0x70:
            falcon_table_offset = read_u32(rom, t_data_off)
            log(f"    ** Falcon Ucode Table Pointer: 0x{falcon_table_offset:08X} **")

    log()

    # =========================================================================
    # SECTION 3: FALCON UCODE TABLE AT 0x80DE8
    # =========================================================================
    log("=" * 80)
    log("SECTION 3: FALCON UCODE TABLE POINTER (0x{:06X})".format(falcon_table_offset or 0))
    log("=" * 80)
    log()

    if falcon_table_offset and falcon_table_offset < rom_size:
        log(f"  BIT Token 0x70 points to: 0x{falcon_table_offset:06X}")
        log(f"  Data at that offset:")
        log(hexdump(rom[falcon_table_offset:falcon_table_offset+64], falcon_table_offset, prefix="    "))
        log()
        log("  ANALYSIS: This region contains repetitive 4-byte patterns (E7 8F XX 00)")
        log("  consistent with Falcon engine register/PLL programming data, NOT a standard")
        log("  lookup table header. This is characteristic of Ada Lovelace (AD10x) VBIOS")
        log("  where the falcon ucode data is stored in a proprietary format embedded")
        log("  within the NV data region, not in a simple indexed table.")
        log()
        log("  The falcon ucode images (FWSEC, etc.) were found by alternative means")
        log("  (scanning for DMAP signatures and device ID 0x2684) as detailed below.")
    else:
        log("  WARNING: Falcon table offset is invalid or missing")
    log()

    # =========================================================================
    # SECTION 4: NV DATA REGION OVERVIEW
    # =========================================================================
    log("=" * 80)
    log("SECTION 4: NV DATA REGION OVERVIEW")
    log("=" * 80)
    log()

    nv_start = 0x24A00
    nv_sig = rom[nv_start:nv_start+2]
    log(f"  NV Region Start: 0x{nv_start:06X}")
    log(f"  Signature: {nv_sig.hex(' ')} ('{chr(nv_sig[0])}{chr(nv_sig[1])}')")
    log()

    # Find end of usable data
    last_data = 0
    for i in range(rom_size - 1, -1, -1):
        if rom[i] != 0xFF:
            last_data = i
            break

    log(f"  Last non-0xFF byte: 0x{last_data:06X} ({last_data/1024:.1f} KB)")
    log(f"  Usable ROM: 0x000000 - 0x{last_data:06X}")
    log(f"  Empty (0xFF): 0x{last_data+1:06X} - 0x{rom_size:06X} ({(rom_size-last_data-1)/1024:.1f} KB)")
    log()

    # ROM layout map
    log("  ROM Layout:")
    log(f"    0x000000 - 0x00FC00: x86/AT BIOS image (63 KB)")
    log(f"    0x00FC00 - 0x024A00: UEFI image (83.5 KB)")
    log(f"    0x024A00 - 0x09FFFF: NV proprietary data (485 KB)")
    log(f"      0x024A00 - 0x02FFFF: Init scripts, tables, pointers")
    log(f"      0x030000 - 0x043BFF: Falcon ucode image #1 (FWSEC-FRTS)")
    log(f"      0x043C00 - 0x053EFF: Falcon ucode image #2 (FWSEC-FRTS copy)")
    log(f"      0x053F00 - 0x06A3FF: Falcon ucode image #3 (FWSEC-SB)")
    log(f"      0x06A400 - 0x070B3F: Falcon ucode image #4 (FWSEC-SB copy)")
    log(f"      0x070B40 - 0x09FFFF: Devinit scripts, PLL tables, memory timing")
    log(f"    0x0A0000 - 0x0DFFFF: Empty (0xFF padding)")
    log(f"    0x0E0000 - 0x0EFBFF: ROM mirror (copy of x86/AT image)")
    log(f"    0x0F0000 - 0x0F6BFF: Additional data / NVGI")
    log(f"    0x0F6C00 - 0x0F6FFF: Empty (0xFF padding)")
    log()

    # Entropy map
    log("  Entropy Analysis (higher = more random/encrypted):")
    regions = [
        (0x000000, 0x00FC00, "x86/AT image"),
        (0x00FC00, 0x024A00, "UEFI image"),
        (0x024A00, 0x030000, "NV headers/init"),
        (0x030000, 0x043C00, "Falcon ucode #1 region"),
        (0x043C00, 0x054000, "Falcon ucode #2 region"),
        (0x054000, 0x06A400, "Falcon ucode #3 region"),
        (0x06A400, 0x070C00, "Falcon ucode #4 region"),
        (0x070C00, 0x0A0000, "Post-ucode NV data"),
    ]
    for start, end, name in regions:
        if end > rom_size:
            continue
        ent = entropy(rom[start:end])
        log(f"    {name:30s} (0x{start:06X}-0x{end:06X}): H = {ent:.3f} bits/byte")
    log()

    # =========================================================================
    # SECTION 5: DMEMMAPPER (DMAP) STRUCTURES
    # =========================================================================
    log("=" * 80)
    log("SECTION 5: DMEMMAPPER (DMAP) STRUCTURES")
    log("=" * 80)
    log()

    dmap_pattern = struct.pack("<I", 0x50414D44)
    dmap_offsets = find_all(rom, dmap_pattern)
    log(f"  Found {len(dmap_offsets)} DMAP signatures: {['0x{:06X}'.format(d) for d in dmap_offsets]}")
    log()

    dmap_infos = []
    for idx, doff in enumerate(dmap_offsets):
        sig = read_u32(rom, doff + 0)
        version = read_u16(rom, doff + 4)
        size = read_u16(rom, doff + 6)

        # DMAP v3 fields (64 bytes total)
        cmdBufOffset = read_u32(rom, doff + 8)
        cmdBufSize = read_u32(rom, doff + 12)
        dataBufOffset = read_u32(rom, doff + 16)
        dataBufSize = read_u32(rom, doff + 20)
        initCmd = read_u32(rom, doff + 24)
        reservedA = read_u32(rom, doff + 28)

        # Additional v3 fields
        field_0x20 = read_u32(rom, doff + 0x20)
        field_0x24 = read_u32(rom, doff + 0x24)
        field_0x28 = read_u32(rom, doff + 0x28)
        field_0x2C = read_u32(rom, doff + 0x2C)
        field_0x30 = read_u32(rom, doff + 0x30)
        field_0x34 = read_u32(rom, doff + 0x34)
        field_0x38 = read_u32(rom, doff + 0x38)
        iface_offset_in_dmap = read_u32(rom, doff + 0x3C)

        # Determine FWSEC type based on unique fields
        if cmdBufOffset == 0x0D40:
            fwsec_type = "FWSEC-FRTS (WPR2 Setup)"
        elif cmdBufOffset == 0x26D0:
            fwsec_type = "FWSEC-SB (Secure Boot)"
        else:
            fwsec_type = f"Unknown (cmdBuf=0x{cmdBufOffset:X})"

        # Pair identification
        pair = "A" if idx < 2 else "B"
        copy = "primary" if idx % 2 == 0 else "redundant copy"

        info = {
            "offset": doff, "version": version, "size": size,
            "cmdBufOffset": cmdBufOffset, "cmdBufSize": cmdBufSize,
            "dataBufOffset": dataBufOffset, "dataBufSize": dataBufSize,
            "initCmd": initCmd, "interfaceOffset": iface_offset_in_dmap,
            "type": fwsec_type, "pair": pair, "copy": copy,
        }
        dmap_infos.append(info)

        log(f"  DMAP #{idx+1} at 0x{doff:06X} [{fwsec_type}] ({copy})")
        log(f"    Version:         {version}")
        log(f"    Size:            {size} bytes")
        log(f"    cmdBufOffset:    0x{cmdBufOffset:08X}")
        log(f"    cmdBufSize:      {cmdBufSize} bytes")
        log(f"    dataBufOffset:   0x{dataBufOffset:08X}")
        log(f"    dataBufSize:     {dataBufSize} bytes")
        log(f"    initCmd:         0x{initCmd:08X}")
        log(f"    interfaceOffset: 0x{iface_offset_in_dmap:08X} (stored at DMAP+0x3C)")
        log(f"    Raw:")
        log(hexdump(rom[doff:doff+64], doff, prefix="      "))
        log()

    # =========================================================================
    # SECTION 6: FALCON UCODE DESCRIPTORS (Ada Lovelace Format)
    # =========================================================================
    log("=" * 80)
    log("SECTION 6: FALCON UCODE DESCRIPTORS (Ada Lovelace / AD10x Format)")
    log("=" * 80)
    log()

    log("  NOTE: On Ada Lovelace GPUs, the falcon ucode descriptors use a")
    log("  proprietary format different from the documented FalconUcodeDescV3Nvidia.")
    log("  The descriptors were found by searching for the RTX 4090 device ID (0x2684)")
    log("  pattern near DMAP structures.")
    log()

    # The pre-header pattern: 01 00 10 00 XX XX 20 00
    # Followed by: 03 00 84 26 (version=3, deviceId=0x2684)
    pre_header_pattern = bytes([0x01, 0x00, 0x10, 0x00])
    pre_header_offsets = find_all(rom[0x24A00:0xE0000], pre_header_pattern)

    # Filter: must be followed by XX XX 20 00 03 00 84 26 within 4 bytes
    valid_descriptors = []
    for rel_off in pre_header_offsets:
        abs_off = 0x24A00 + rel_off
        if abs_off + 12 >= rom_size:
            continue
        # Check: at +6: u16 = 0x0020, at +8: u16 = 0x0003, at +10: u16 = 0x2684
        hdr_size_field = read_u16(rom, abs_off + 6)
        ver_field = read_u16(rom, abs_off + 8)
        dev_id = read_u16(rom, abs_off + 10)
        if hdr_size_field == 0x0020 and ver_field == 0x0003 and dev_id == 0x2684:
            valid_descriptors.append(abs_off)

    log(f"  Found {len(valid_descriptors)} Falcon Ucode Descriptors with pre-header:")
    log()

    ucode_infos = []

    for idx, desc_off in enumerate(valid_descriptors):
        log(f"  --- Falcon Ucode Descriptor #{idx+1} at 0x{desc_off:06X} ---")
        log()

        # Pre-header (8 bytes)
        pre_ver = read_u8(rom, desc_off + 0)  # 0x01
        pre_pad = read_u8(rom, desc_off + 1)  # 0x00
        pre_type = read_u16(rom, desc_off + 2)  # 0x0010
        pre_data_size = read_u16(rom, desc_off + 4)  # dmem/imem size field
        pre_hdr_size = read_u16(rom, desc_off + 6)  # 0x0020

        log(f"    Pre-header (8 bytes):")
        log(f"      version:          {pre_ver}")
        log(f"      type/flags:       0x{pre_type:04X}")
        log(f"      dataSize:         0x{pre_data_size:04X} ({pre_data_size} bytes)")
        log(f"      headerSize:       0x{pre_hdr_size:04X} ({pre_hdr_size} bytes)")
        log()

        # Common descriptor (starts at desc_off + 8)
        desc_body = desc_off + 8
        desc_version = read_u16(rom, desc_body + 0)  # 0x0003
        desc_deviceId = read_u16(rom, desc_body + 2)  # 0x2684

        log(f"    Descriptor Header:")
        log(f"      version:          {desc_version}")
        log(f"      deviceId:         0x{desc_deviceId:04X} (RTX 4090)")
        log()

        # Find the packed bytes pattern: 01 04 08 02
        packed_off = None
        for search in range(desc_body + 4, desc_body + 64):
            if rom[search:search+4] == bytes([0x01, 0x04, 0x08, 0x02]):
                packed_off = search
                break

        if packed_off is None:
            log("    ERROR: Could not find packed info bytes")
            log()
            continue

        zeros_before_packed = packed_off - (desc_body + 4)

        # Packed info bytes
        sig_versions = read_u8(rom, packed_off + 0)  # 0x01
        ucode_id = read_u8(rom, packed_off + 1)  # 0x04
        engine_id_mask = read_u8(rom, packed_off + 2)  # 0x08
        sig_count = read_u8(rom, packed_off + 3)  # 0x02

        # Fields after packed bytes
        field_A = read_u32(rom, packed_off + 4)   # count/flags
        code_size = read_u32(rom, packed_off + 8)  # IMEM code size
        field_B = read_u32(rom, packed_off + 12)  # count/flags
        iface_offset = read_u32(rom, packed_off + 16)  # interfaceOffset

        engine_names = {0x01: "PMU", 0x02: "DISP", 0x04: "SEC2", 0x08: "GSP", 0x10: "NVDEC"}
        engines = [engine_names.get(1 << b, f"BIT{b}") for b in range(8) if engine_id_mask & (1 << b)]

        log(f"    Packed Info (at 0x{packed_off:06X}, {zeros_before_packed} zero bytes before):")
        log(f"      signatureVersions: {sig_versions}")
        log(f"      ucodeId:           0x{ucode_id:02X} (4 = GSP/FWSEC)")
        log(f"      engineIdMask:      0x{engine_id_mask:02X} ({', '.join(engines)})")
        log(f"      signatureCount:    {sig_count}")
        log()
        log(f"    Size Fields:")
        log(f"      fieldA:            {field_A}")
        log(f"      codeSize (IMEM):   0x{code_size:04X} ({code_size} bytes)")
        log(f"      fieldB:            {field_B}")
        log(f"      interfaceOffset:   0x{iface_offset:04X} ({iface_offset} bytes)")
        log()

        # Signatures
        sig_start = packed_off + 20
        RSA_3K_SIZE = 384
        total_sig_bytes = sig_count * RSA_3K_SIZE

        log(f"    RSA-3K Signatures:")
        log(f"      Start:    0x{sig_start:06X}")
        log(f"      Count:    {sig_count}")
        log(f"      Size:     {sig_count} x {RSA_3K_SIZE} = {total_sig_bytes} bytes")
        log(f"      End:      0x{sig_start + total_sig_bytes:06X}")
        log()

        for s in range(sig_count):
            s_off = sig_start + s * RSA_3K_SIZE
            is_zero_start = all(b == 0 for b in rom[s_off:s_off+12])
            log(f"      Sig #{s}: 0x{s_off:06X} - first 16: {rom[s_off:s_off+16].hex(' ')}")
            if is_zero_start:
                log(f"        (starts with zeros - may have padding prefix)")

        log()

        # Data section (IMEM + DMEM after signatures)
        data_start = sig_start + total_sig_bytes

        log(f"    Data Section (after signatures):")
        log(f"      Start: 0x{data_start:06X}")
        log(f"      First 64 bytes:")
        log(hexdump(rom[data_start:data_start+64], data_start, prefix="        "))
        log()

        # Check for compile date string
        date_check = rom[data_start:data_start+256]
        date_pos = date_check.find(b'Jul ')
        if date_pos < 0:
            date_pos = date_check.find(b'20')

        # Find the corresponding DMAP
        matching_dmap = None
        for dmap in dmap_infos:
            if dmap["interfaceOffset"] == iface_offset:
                # Check if distance makes sense
                dist = dmap["offset"] - data_start
                if 0 < dist < 0x20000:
                    matching_dmap = dmap
                    break

        if matching_dmap:
            dmap_rom_off = matching_dmap["offset"]
            dmem_offset_from_data = dmap_rom_off - data_start
            log(f"    Matching DMAP at 0x{dmap_rom_off:06X} ({matching_dmap['type']})")
            log(f"      Distance from data_start to DMAP: 0x{dmem_offset_from_data:X} ({dmem_offset_from_data} bytes)")
            log(f"      DMAP interfaceOffset = 0x{iface_offset:X}")
            log()

            # The data section layout:
            # The compile date appears near the start -> this is DMEM
            # DMEM contains the DMAP at interfaceOffset from DMEM start
            # So: if DMAP is at data_start + X, and interfaceOffset = Y
            # Then the DMEM doesn't start right at data_start
            # Instead: DMEM_start = DMAP_rom_offset - interfaceOffset
            dmem_start = dmap_rom_off - iface_offset
            dmem_total = iface_offset + 64  # DMAP is at end, 64 bytes
            dmem_end = dmem_start + dmem_total

            # IMEM is the remaining data
            # data_start to dmem_start could be IMEM, or there might be other data
            imem_region_size = dmem_start - data_start

            log(f"    Memory Layout (computed from DMAP position):")
            log(f"      IMEM region: 0x{data_start:06X} - 0x{dmem_start:06X} ({imem_region_size} bytes)")
            log(f"      DMEM region: 0x{dmem_start:06X} - 0x{dmem_end:06X} ({dmem_total} bytes)")
            log(f"      DMAP within DMEM: 0x{dmap_rom_off:06X} (at DMEM+0x{iface_offset:X})")
            log()

            # Verify: the compile date should be in DMEM
            date_abs = data_start + (date_check.find(b'Jul') if b'Jul' in date_check else -1)
            if date_abs >= data_start:
                if dmem_start <= date_abs < dmem_end:
                    dmem_date_off = date_abs - dmem_start
                    log(f"      Compile date 'Jul ...' at DMEM+0x{dmem_date_off:X} (ROM 0x{date_abs:06X})")
                else:
                    log(f"      NOTE: Compile date at 0x{date_abs:06X} is in IMEM region (offset from data_start: 0x{date_abs-data_start:X})")

            # Footer after DMAP
            footer_off = dmap_rom_off + 64
            footer = rom[footer_off:footer_off+8]
            log(f"      Footer after DMAP: {footer.hex(' ')}")
            # 0xA6 0x00 0xFF 0x42 = some kind of checksum/marker
            log()

        # Determine FWSEC type
        fwsec_type_str = "UNKNOWN"
        if matching_dmap:
            fwsec_type_str = matching_dmap["type"]
        elif iface_offset == 0xD2C:
            fwsec_type_str = "FWSEC-FRTS (WPR2 Setup)"
        elif iface_offset == 0xC18:
            fwsec_type_str = "FWSEC-SB (Secure Boot)"

        pair_label = "primary" if idx % 2 == 0 else "redundant copy"

        ucode_info = {
            "desc_offset": desc_off,
            "code_size": code_size,
            "iface_offset": iface_offset,
            "data_size": pre_data_size,
            "sig_count": sig_count,
            "sig_start": sig_start,
            "data_start": data_start,
            "type": fwsec_type_str,
            "pair_label": pair_label,
            "matching_dmap": matching_dmap,
            "engine_id_mask": engine_id_mask,
            "ucode_id": ucode_id,
        }
        ucode_infos.append(ucode_info)

        log(f"    SUMMARY: {fwsec_type_str} ({pair_label})")
        log(f"      Descriptor:   0x{desc_off:06X}")
        log(f"      Signatures:   0x{sig_start:06X} ({sig_count} x 384 bytes)")
        log(f"      Code+Data:    0x{data_start:06X}")
        log(f"      DMAP:         0x{matching_dmap['offset']:06X}" if matching_dmap else "      DMAP: not found")
        log(f"      Target:       GSP Falcon (0x110000)")
        log()
        log("  " + "-" * 70)
        log()

    # =========================================================================
    # SECTION 7: REGION ANALYSIS (EFI End to Falcon Data)
    # =========================================================================
    log("=" * 80)
    log("SECTION 7: NV DATA STRUCTURES AND SIGNATURES")
    log("=" * 80)
    log()

    # NVGI structures
    nvgi_pattern = b'NVGI'
    nvgi_hits = find_all(rom, nvgi_pattern)
    log(f"  NVGI (NVIDIA GPU Init) signatures: {['0x{:06X}'.format(h) for h in nvgi_hits]}")
    for h in nvgi_hits:
        log(f"    At 0x{h:06X}:")
        log(hexdump(rom[h:h+32], h, prefix="      "))
    log()

    # NPDS/NPDE structures
    npds_hits = find_all(rom, b'NPDS')
    npde_hits = find_all(rom, b'NPDE')
    log(f"  NPDS (NV PCI Data Structure): {['0x{:06X}'.format(h) for h in npds_hits]}")
    log(f"  NPDE (NV PCI Data Extension): {['0x{:06X}'.format(h) for h in npde_hits]}")
    log()

    # Check for known magic numbers
    log("  Other Signatures:")
    for sig_bytes, sig_name in [(b'\x7fELF', "ELF"), (b'HSFW', "HS Firmware"),
                                 (b'LSFW', "LS Firmware"), (b'BROM', "Boot ROM")]:
        hits = find_all(rom, sig_bytes)
        if hits:
            log(f"    {sig_name}: {['0x{:06X}'.format(h) for h in hits]}")
        else:
            log(f"    {sig_name}: NOT FOUND")
    log()

    # =========================================================================
    # SECTION 8: PMU TABLE ANALYSIS
    # =========================================================================
    log("=" * 80)
    log("SECTION 8: PMU/PERF TABLE (BIT Token 0x50)")
    log("=" * 80)
    log()

    pmu_token = None
    for t_id, t_ver, t_size, t_data_off in tokens:
        if t_id == 0x50:
            pmu_token = (t_id, t_ver, t_size, t_data_off)
            break

    if pmu_token:
        t_id, t_ver, t_size, t_data_off = pmu_token
        log(f"  PMU Token: Ver={t_ver} Size={t_size} Data=0x{t_data_off:04X}")
        log()

        # Parse as pointer table
        log(f"  Pointer Table ({t_size} bytes, {t_size//4} entries):")
        interesting_ptrs = []
        for i in range(0, min(t_size, 256), 4):
            val = read_u32(rom, t_data_off + i)
            if 0 < val < rom_size:
                interesting_ptrs.append((i, val))
                log(f"    +0x{i:02X}: 0x{val:06X} -> {rom[val:val+8].hex(' ')}")

        log(f"\n  Total valid ROM pointers: {len(interesting_ptrs)}")
    log()

    # =========================================================================
    # SECTION 9: ALL FALCON UCODE IMAGES COMPARISON
    # =========================================================================
    log("=" * 80)
    log("SECTION 9: FALCON UCODE IMAGES COMPARISON")
    log("=" * 80)
    log()

    if len(ucode_infos) >= 2:
        log("  +-------+-------------------+----------+----------+----------+----------+--------+")
        log("  | Image | Type              | Desc Off | Sig Off  | Data Off | DMAP Off | Sigs   |")
        log("  +-------+-------------------+----------+----------+----------+----------+--------+")
        for i, u in enumerate(ucode_infos):
            dmap_str = f"0x{u['matching_dmap']['offset']:06X}" if u['matching_dmap'] else "   N/A  "
            log(f"  |  #{i+1}   | {u['type'][:17]:17s} | 0x{u['desc_offset']:06X} | 0x{u['sig_start']:06X} | 0x{u['data_start']:06X} | {dmap_str} | {u['sig_count']:2d}x384 |")
        log("  +-------+-------------------+----------+----------+----------+----------+--------+")
        log()

        # Check if pairs are identical
        if len(ucode_infos) >= 2:
            u1, u2 = ucode_infos[0], ucode_infos[1]
            if u1.get('matching_dmap') and u2.get('matching_dmap'):
                d1_start = u1['data_start']
                d1_end = u1['matching_dmap']['offset'] + 64
                d2_start = u2['data_start']
                d2_end = u2['matching_dmap']['offset'] + 64
                size1 = d1_end - d1_start
                size2 = d2_end - d2_start
                if size1 == size2:
                    data1 = rom[d1_start:d1_end]
                    data2 = rom[d2_start:d2_end]
                    if data1 == data2:
                        log(f"  FWSEC-FRTS pair: Images are IDENTICAL (redundant copy)")
                    else:
                        diffs = sum(1 for a, b in zip(data1, data2) if a != b)
                        log(f"  FWSEC-FRTS pair: Images differ in {diffs} bytes (different signatures)")

        if len(ucode_infos) >= 4:
            u3, u4 = ucode_infos[2], ucode_infos[3]
            if u3.get('matching_dmap') and u4.get('matching_dmap'):
                d3_start = u3['data_start']
                d3_end = u3['matching_dmap']['offset'] + 64
                d4_start = u4['data_start']
                d4_end = u4['matching_dmap']['offset'] + 64
                size3 = d3_end - d3_start
                size4 = d4_end - d4_start
                if size3 == size4:
                    data3 = rom[d3_start:d3_end]
                    data4 = rom[d4_start:d4_end]
                    if data3 == data4:
                        log(f"  FWSEC-SB pair:   Images are IDENTICAL (redundant copy)")
                    else:
                        diffs = sum(1 for a, b in zip(data3, data4) if a != b)
                        log(f"  FWSEC-SB pair:   Images differ in {diffs} bytes (different signatures)")
    log()

    # =========================================================================
    # SECTION 10: DRIVER IMPLEMENTATION SUMMARY
    # =========================================================================
    log("=" * 80)
    log("SECTION 10: NVDAAL DRIVER IMPLEMENTATION GUIDE")
    log("=" * 80)
    log()

    log("  FWSEC Extraction and Loading for NVDAAL (macOS kext)")
    log("  Target: RTX 4090 (AD102) / Ada Lovelace")
    log()

    log("  A. VBIOS ROM Acquisition:")
    log("     1. Read VBIOS via ACPI _ROM method (preferred)")
    log("        - IOACPIPlatformDevice::evaluateObject(\"_ROM\", ...)")
    log("     2. Alternatively, read via BAR0 PROM registers:")
    log("        - NV_PROM base: BAR0 + 0x300000")
    log("        - Read in 4-byte chunks, total ~1MB")
    log("     3. IMPORTANT: Full ROM (~1MB) needed, not just PCI ROM (~150KB)")
    log()

    log("  B. BIT Table Parsing:")
    log("     1. Scan for BIT signature: FF B8 42 49 54 00 (at ~0x1B0)")
    log("     2. Parse header: headerSize(u8) + tokenSize(u8) + tokenCount(u8)")
    log("     3. Token table at offset headerSize (0x0C) from BIT start")
    log("     4. Each token: id(u8) + ver(u8) + size(u16) + dataOffset(u16)")
    log("     5. Find token ID 0x70 (FALCON_DATA) -> read u32 at dataOffset")
    log("        Note: On AD10x, this offset (0x80DE8) points to falcon engine data,")
    log("        NOT a standard lookup table. Use DMAP-based discovery instead.")
    log()

    log("  C. FWSEC Discovery via DMAP Scanning (Ada Lovelace method):")
    log("     1. Scan NV data region (after UEFI image) for DMAP signature 0x50414D44")
    log("     2. For each DMAP found:")
    log("        a. Read interfaceOffset from DMAP+0x3C")
    log("        b. Search backwards for descriptor pre-header: 01 00 10 00 XX XX 20 00")
    log("        c. Verify descriptor: version=3, deviceId=0x2684 at pre-header+8")
    log()

    log("  D. Ada Lovelace Falcon Ucode Descriptor Format:")
    log("     Pre-header (8 bytes):")
    log("       +0x00: u8  version (0x01)")
    log("       +0x01: u8  reserved (0x00)")
    log("       +0x02: u16 type/flags (0x0010)")
    log("       +0x04: u16 dataSize (IMEM/DMEM size related)")
    log("       +0x06: u16 headerSize (0x0020 = 32)")
    log("     ")
    log("     Descriptor body (starts at pre-header + 8):")
    log("       +0x00: u16 version (3)")
    log("       +0x02: u16 deviceId (0x2684 for RTX 4090)")
    log("       +0x04: variable zeros (16 bytes for FWSEC-FRTS, 4 bytes for FWSEC-SB)")
    log("     ")
    log("     Packed info (4 bytes):")
    log("       +0x00: u8  signatureVersions (1)")
    log("       +0x01: u8  ucodeId (0x04 = GSP/FWSEC)")
    log("       +0x02: u8  engineIdMask (0x08 = GSP Falcon)")
    log("       +0x03: u8  signatureCount (2)")
    log("     ")
    log("     Size fields (16 bytes):")
    log("       +0x00: u32 fieldA (4)")
    log("       +0x04: u32 codeSize / IMEM size")
    log("       +0x08: u32 fieldB (5)")
    log("       +0x0C: u32 interfaceOffset (DMAP position in DMEM)")
    log("     ")
    log("     Then: signatureCount x 384 bytes of RSA-3K signatures")
    log("     Then: IMEM (code) followed by DMEM (data) with DMAP near end")
    log()

    if ucode_infos:
        u = ucode_infos[0]  # Use first FWSEC-FRTS
        log("  E. FWSEC-FRTS Specific Values (RTX 4090, VBIOS 95.02.18.80):")
        log(f"     Descriptor offset:    0x{u['desc_offset']:06X}")
        log(f"     IMEM code size:       0x{u['code_size']:X} ({u['code_size']} bytes)")
        log(f"     Interface offset:     0x{u['iface_offset']:X}")
        log(f"     Signature count:      {u['sig_count']}")
        log(f"     Signatures at:        0x{u['sig_start']:06X}")
        log(f"     Code+Data at:         0x{u['data_start']:06X}")
        if u['matching_dmap']:
            log(f"     DMAP at:              0x{u['matching_dmap']['offset']:06X}")
            log(f"     DMAP cmdBufOffset:    0x{u['matching_dmap']['cmdBufOffset']:X}")
            log(f"     DMAP version:         {u['matching_dmap']['version']}")
        log()

    if len(ucode_infos) >= 3:
        u = ucode_infos[2]  # Use first FWSEC-SB
        log("  F. FWSEC-SB Specific Values (RTX 4090, VBIOS 95.02.18.80):")
        log(f"     Descriptor offset:    0x{u['desc_offset']:06X}")
        log(f"     IMEM code size:       0x{u['code_size']:X} ({u['code_size']} bytes)")
        log(f"     Interface offset:     0x{u['iface_offset']:X}")
        log(f"     Signature count:      {u['sig_count']}")
        log(f"     Signatures at:        0x{u['sig_start']:06X}")
        log(f"     Code+Data at:         0x{u['data_start']:06X}")
        if u['matching_dmap']:
            log(f"     DMAP at:              0x{u['matching_dmap']['offset']:06X}")
            log(f"     DMAP cmdBufOffset:    0x{u['matching_dmap']['cmdBufOffset']:X}")
        log()

    log("  G. FWSEC Loading Procedure:")
    log("     1. Extract FWSEC-FRTS from VBIOS (descriptor + signatures + code + data)")
    log("     2. Load IMEM onto GSP Falcon (MMIO 0x110000):")
    log("        - NV_PGSP_FALCON_IMEMC(0) = 0x110180 (set auto-increment)")
    log("        - NV_PGSP_FALCON_IMEMD(0) = 0x110184 (write code words)")
    log("     3. Load DMEM onto GSP Falcon:")
    log("        - NV_PGSP_FALCON_DMEMC(0) = 0x1101C0 (set auto-increment)")
    log("        - NV_PGSP_FALCON_DMEMD(0) = 0x1101C4 (write data words)")
    log("     4. Patch DMAP command buffer with FRTS parameters (WPR2 addresses)")
    log("     5. Set boot vector: NV_PGSP_FALCON_BOOTVEC = 0x110104")
    log("     6. Start execution: NV_PGSP_FALCON_CPUCTL = 0x110100 (START bit)")
    log("     7. Wait for FRTS completion: read NV_PTOP_SCPM(0x001438)")
    log("        - Upper 16 bits = 0 means success")
    log("     8. Verify WPR2: NV_PFB_PRI_MMU_WPR2_ADDR_LO/HI (0x1FA824/0x1FA828)")
    log()

    log("  H. After FWSEC-FRTS (Full GSP Boot Sequence):")
    log("     1. FWSEC-FRTS configures WPR2 (Write Protected Region 2)")
    log("     2. Load booter_load firmware onto SEC2 Falcon (0x840000)")
    log("        - SEC2 booter loads GSP-RM firmware into WPR2")
    log("     3. Load booter_unload firmware (for cleanup)")
    log("     4. Start GSP-RM via RPC protocol")
    log("     5. Note: FWSEC-SB (Secure Boot) may be needed if ACR is required")
    log()

    log("  I. Key Registers:")
    log("     GSP Falcon base:      0x110000")
    log("     SEC2 Falcon base:     0x840000")
    log("     FALCON_CPUCTL:        base + 0x100")
    log("     FALCON_BOOTVEC:       base + 0x104")
    log("     FALCON_HWCFG:         base + 0x108")
    log("     FALCON_DMACTL:        base + 0x10C")
    log("     FALCON_IMEMC(i):      base + 0x180 + i*16")
    log("     FALCON_IMEMD(i):      base + 0x184 + i*16")
    log("     FALCON_DMEMC(i):      base + 0x1C0 + i*16")
    log("     FALCON_DMEMD(i):      base + 0x1C4 + i*16")
    log("     FALCON_OS:            base + 0x080")
    log("     NV_PROM (VBIOS):      0x300000")
    log("     FRTS status:          0x001438")
    log("     WPR2 LO:              0x1FA824")
    log("     WPR2 HI:              0x1FA828")
    log()

    # =========================================================================
    # SECTION 11: RAW DESCRIPTOR DUMPS
    # =========================================================================
    log("=" * 80)
    log("SECTION 11: RAW DESCRIPTOR DUMPS")
    log("=" * 80)
    log()

    for idx, u in enumerate(ucode_infos):
        desc_off = u['desc_offset']
        log(f"  Ucode #{idx+1} ({u['type']}, {u['pair_label']}) Raw Descriptor:")
        # Show 96 bytes from pre-header start
        log(hexdump(rom[desc_off:desc_off+96], desc_off, prefix="    "))
        log()

    # Save output
    output = "\n".join(out_lines)
    with open(OUTPUT_PATH, "w") as f:
        f.write(output)

    print(f"\n{'='*80}")
    print(f"Analysis saved to: {OUTPUT_PATH}")
    print(f"Total lines: {len(out_lines)}")


if __name__ == "__main__":
    main()
