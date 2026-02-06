#!/usr/bin/env python3
"""
Extract FWSEC firmware from NVGI container (AD102.rom)

The NVGI container has:
  - Header (0x00-0x9400): NVGI signature + crypto signatures
  - VBIOS data (0x9400+): expansion ROM images (x86, EFI, FWSEC)

The FWSEC image uses NPDS/NPDE format instead of standard PCIR.
We extract the V3 ucode descriptor + signatures + IMEM + DMEM.

Output: fwsec.bin with a simple header for the EFI driver to consume.
"""

import struct
import sys
import os

NVGI_FILE = os.path.join(os.path.dirname(__file__), "AD102.rom")
OUTPUT_FILE = os.path.join(os.path.dirname(__file__), "fwsec.bin")

# V3 descriptor size
FALCON_UCODE_DESC_V3_SIZE = 44

# VDesc field masks
VDESC_FLAGS_VERSION_BIT = 0x00000001
VDESC_VERSION_SHIFT = 8
VDESC_VERSION_MASK = 0x0000FF00
VDESC_SIZE_SHIFT = 16
VDESC_SIZE_MASK = 0xFFFF0000

def read_u8(data, off):
    return data[off]

def read_u16(data, off):
    return struct.unpack_from('<H', data, off)[0]

def read_u32(data, off):
    return struct.unpack_from('<I', data, off)[0]

def hexdump(data, offset, length, prefix=""):
    for i in range(0, min(length, len(data) - offset), 16):
        addr = offset + i
        chunk = data[addr:addr+16]
        hex_str = ' '.join(f'{b:02X}' for b in chunk)
        print(f"{prefix}{addr:06X}: {hex_str}")

def scan_rom_images(data):
    """Scan for all 0xAA55 ROM image headers"""
    images = []
    offset = 0
    while offset < len(data) - 2:
        sig = read_u16(data, offset)
        if sig == 0xAA55:
            # Found ROM header, check for PCIR or NPDS
            pcir_offset = read_u16(data, offset + 0x18)
            abs_pcir = offset + pcir_offset

            if abs_pcir + 4 <= len(data):
                pcir_sig = data[abs_pcir:abs_pcir+4]

                if pcir_sig == b'PCIR':
                    vendor = read_u16(data, abs_pcir + 4)
                    device = read_u16(data, abs_pcir + 6)
                    img_len = read_u16(data, abs_pcir + 16) * 512
                    code_type = read_u8(data, abs_pcir + 20)
                    indicator = read_u8(data, abs_pcir + 21)
                    images.append({
                        'offset': offset,
                        'pcir_offset': abs_pcir,
                        'format': 'PCIR',
                        'vendor': vendor,
                        'device': device,
                        'image_length': img_len,
                        'code_type': code_type,
                        'last_image': bool(indicator & 0x80),
                    })
                    if img_len > 0:
                        offset += img_len
                        continue

                elif pcir_sig == b'NPDS':
                    # NVIDIA Packed Data Section
                    npds_size = read_u16(data, abs_pcir + 4)
                    npds_rev = read_u8(data, abs_pcir + 6)

                    # Look for NPDE after NPDS
                    npde_off = abs_pcir + npds_size
                    if npde_off + 4 <= len(data) and data[npde_off:npde_off+4] == b'NPDE':
                        npde_img_len = read_u16(data, npde_off + 4) * 512
                        npde_code_type_byte = read_u8(data, abs_pcir + 0x14)  # codeType in NPDS
                        npde_indicator = read_u8(data, npde_off + 6)

                        # Also check for code type indicator in NPDS extension area
                        # The NPDS at 0x033E20 has the 0xE0 at a specific offset
                        code_type = 0xFF  # unknown

                        # Scan NPDS for code type indicators
                        for scan_off in range(abs_pcir, min(abs_pcir + npds_size, len(data) - 1)):
                            if data[scan_off] == 0xE0:
                                code_type = 0xE0
                                break

                        # Get actual data size from NPDE
                        npde_data_size = read_u32(data, npde_off + 8) if npde_off + 12 <= len(data) else 0

                        images.append({
                            'offset': offset,
                            'pcir_offset': abs_pcir,
                            'format': 'NPDS',
                            'npds_size': npds_size,
                            'npde_offset': npde_off,
                            'npde_img_len': npde_img_len,
                            'npde_data_size': npde_data_size,
                            'code_type': code_type,
                            'last_image': bool(npde_indicator & 0x80),
                        })

                        if npde_img_len > 0:
                            offset += npde_img_len
                            continue
                else:
                    # Unknown format, try to find code type info
                    images.append({
                        'offset': offset,
                        'pcir_offset': abs_pcir,
                        'format': f'UNKNOWN({pcir_sig})',
                        'code_type': 0xFF,
                    })

        offset += 0x200  # Scan in 512-byte increments

    return images

def find_v3_descriptor(data, start, length):
    """Search for a valid FALCON_UCODE_DESC_V3 within data"""
    results = []

    for off in range(start, min(start + length - FALCON_UCODE_DESC_V3_SIZE, len(data)), 4):
        vdesc = read_u32(data, off)

        # Check VDesc: bit 0 must be set (version available)
        if not (vdesc & VDESC_FLAGS_VERSION_BIT):
            continue

        # Version must be V3 (3)
        version = (vdesc & VDESC_VERSION_MASK) >> VDESC_VERSION_SHIFT
        if version != 3:
            continue

        # Size should be reasonable (V3 desc + signatures)
        desc_total_size = (vdesc & VDESC_SIZE_MASK) >> VDESC_SIZE_SHIFT
        if desc_total_size < FALCON_UCODE_DESC_V3_SIZE or desc_total_size > 0x10000:
            continue

        # Parse the full V3 descriptor
        stored_size = read_u32(data, off + 4)
        pkc_offset = read_u32(data, off + 8)
        iface_offset = read_u32(data, off + 12)
        imem_phys_base = read_u32(data, off + 16)
        imem_load_size = read_u32(data, off + 20)
        imem_virt_base = read_u32(data, off + 24)
        dmem_phys_base = read_u32(data, off + 28)
        dmem_load_size = read_u32(data, off + 32)
        engine_id_mask = read_u16(data, off + 36)
        ucode_id = read_u8(data, off + 38)
        sig_count = read_u8(data, off + 39)
        sig_versions = read_u16(data, off + 40)

        # Validate: IMEM and DMEM sizes should be reasonable
        if imem_load_size == 0 or imem_load_size > 0x100000:
            continue
        if dmem_load_size == 0 or dmem_load_size > 0x100000:
            continue
        if stored_size == 0 or stored_size > 0x100000:
            continue

        # Check that stored_size >= imem + dmem
        if stored_size < imem_load_size + dmem_load_size:
            continue

        results.append({
            'offset': off,
            'vdesc': vdesc,
            'desc_total_size': desc_total_size,
            'stored_size': stored_size,
            'pkc_offset': pkc_offset,
            'iface_offset': iface_offset,
            'imem_phys_base': imem_phys_base,
            'imem_load_size': imem_load_size,
            'imem_virt_base': imem_virt_base,
            'dmem_phys_base': dmem_phys_base,
            'dmem_load_size': dmem_load_size,
            'engine_id_mask': engine_id_mask,
            'ucode_id': ucode_id,
            'sig_count': sig_count,
            'sig_versions': sig_versions,
        })

    return results

def main():
    print(f"=== FWSEC Extraction from NVGI Container ===")
    print(f"Input: {NVGI_FILE}")
    print()

    with open(NVGI_FILE, 'rb') as f:
        data = f.read()

    print(f"NVGI size: {len(data)} bytes (0x{len(data):X})")

    # Check NVGI header
    nvgi_sig = data[0:4]
    print(f"Header signature: {nvgi_sig}")

    # The NVGI header is 0x9400 bytes (crypto signatures)
    nvgi_header_size = 0x9400
    print(f"NVGI header size: 0x{nvgi_header_size:X}")
    print(f"VBIOS data starts at: 0x{nvgi_header_size:X}")
    print()

    # Scan for ROM images in the full NVGI
    print("=== Scanning for ROM images in full NVGI ===")
    images = scan_rom_images(data)

    for i, img in enumerate(images):
        ct_name = {0x00: 'x86', 0x03: 'EFI', 0xE0: 'FWSEC'}.get(img['code_type'], f'0x{img["code_type"]:02X}')
        print(f"  Image {i}: offset=0x{img['offset']:06X}, format={img['format']}, codeType={ct_name}", end='')
        if 'image_length' in img:
            print(f", len=0x{img['image_length']:X}", end='')
        if 'npde_data_size' in img:
            print(f", dataSize=0x{img['npde_data_size']:X}", end='')
        if img.get('last_image'):
            print(" [LAST]", end='')
        print()

    print()

    # Now scan for V3 descriptors in the entire NVGI
    print("=== Searching for V3 ucode descriptors ===")
    v3_descs = find_v3_descriptor(data, 0, len(data))

    if not v3_descs:
        print("ERROR: No V3 descriptors found!")
        # Try to find any descriptor pattern
        print("\nSearching for any VDesc with version bit set...")
        for off in range(0, len(data) - 4, 4):
            vdesc = read_u32(data, off)
            if (vdesc & 0x01) and ((vdesc >> 8) & 0xFF) in [2, 3]:
                size_field = (vdesc >> 16) & 0xFFFF
                if 44 <= size_field <= 0x8000:
                    print(f"  Potential VDesc at 0x{off:06X}: 0x{vdesc:08X} (v{(vdesc>>8)&0xFF}, size={size_field})")
                    hexdump(data, off, 48, "    ")
        sys.exit(1)

    for desc in v3_descs:
        print(f"\n  V3 Descriptor at 0x{desc['offset']:06X}:")
        print(f"    VDesc:          0x{desc['vdesc']:08X}")
        print(f"    DescTotalSize:  0x{desc['desc_total_size']:X} ({desc['desc_total_size']} bytes)")
        print(f"    StoredSize:     0x{desc['stored_size']:X} ({desc['stored_size']} bytes)")
        print(f"    PKCDataOffset:  0x{desc['pkc_offset']:X}")
        print(f"    InterfaceOff:   0x{desc['iface_offset']:X}")
        print(f"    IMEM PhysBase:  0x{desc['imem_phys_base']:X}")
        print(f"    IMEM LoadSize:  0x{desc['imem_load_size']:X} ({desc['imem_load_size']} bytes)")
        print(f"    IMEM VirtBase:  0x{desc['imem_virt_base']:X}")
        print(f"    DMEM PhysBase:  0x{desc['dmem_phys_base']:X}")
        print(f"    DMEM LoadSize:  0x{desc['dmem_load_size']:X} ({desc['dmem_load_size']} bytes)")
        print(f"    EngineIdMask:   0x{desc['engine_id_mask']:04X}")
        print(f"    UcodeId:        {desc['ucode_id']}")
        print(f"    SigCount:       {desc['sig_count']}")
        print(f"    SigVersions:    0x{desc['sig_versions']:04X}")

        # Calculate payload layout
        sig_total_size = desc['desc_total_size'] - FALCON_UCODE_DESC_V3_SIZE
        image_offset = desc['offset'] + desc['desc_total_size']
        total_payload = desc['desc_total_size'] + desc['stored_size']

        print(f"    --- Layout ---")
        print(f"    Descriptor:  0x{desc['offset']:06X} - 0x{desc['offset'] + FALCON_UCODE_DESC_V3_SIZE:06X} ({FALCON_UCODE_DESC_V3_SIZE} bytes)")
        print(f"    Signatures:  0x{desc['offset'] + FALCON_UCODE_DESC_V3_SIZE:06X} - 0x{image_offset:06X} ({sig_total_size} bytes)")
        print(f"    IMEM:        0x{image_offset:06X} - 0x{image_offset + desc['imem_load_size']:06X} ({desc['imem_load_size']} bytes)")
        print(f"    DMEM:        0x{image_offset + desc['imem_load_size']:06X} - 0x{image_offset + desc['imem_load_size'] + desc['dmem_load_size']:06X} ({desc['dmem_load_size']} bytes)")
        print(f"    Total:       {total_payload} bytes (0x{total_payload:X})")

    # Select the correct FWSEC descriptor
    # NVIDIA's s_fwsecUcodeId = 9 for FWSEC (handles both FRTS and SB commands)
    # UcodeId=10 is a different firmware (SEC2 scrubber or similar)
    FWSEC_UCODE_ID = 9
    best_desc = None
    for desc in v3_descs:
        if desc['ucode_id'] == FWSEC_UCODE_ID:
            best_desc = desc
            break

    if best_desc is None:
        print(f"WARNING: UcodeId={FWSEC_UCODE_ID} not found, using first descriptor")
        best_desc = v3_descs[0]

    print(f"\n=== Extracting FWSEC using descriptor at 0x{best_desc['offset']:06X} ===")

    desc_offset = best_desc['offset']
    desc_total_size = best_desc['desc_total_size']
    stored_size = best_desc['stored_size']

    # The complete FWSEC blob: [V3 Descriptor][Signatures][IMEM][DMEM]
    total_size = desc_total_size + stored_size

    if desc_offset + total_size > len(data):
        print(f"ERROR: FWSEC data extends beyond file (need 0x{desc_offset + total_size:X}, have 0x{len(data):X})")
        sys.exit(1)

    fwsec_blob = data[desc_offset:desc_offset + total_size]

    # Create output with a simple header:
    # Magic: "FWSC" (4 bytes)
    # Version: 1 (4 bytes)
    # DescSize: size of V3 descriptor (4 bytes)
    # TotalDescSize: desc_total_size including signatures (4 bytes)
    # StoredSize: size of IMEM+DMEM data (4 bytes)
    # Reserved: 12 bytes (zero)
    # [V3 Descriptor][Signatures][IMEM][DMEM]

    HEADER_SIZE = 32
    header = struct.pack('<4sIIIII8x',
        b'FWSC',           # Magic
        1,                  # Version
        FALCON_UCODE_DESC_V3_SIZE,  # V3 descriptor size
        desc_total_size,    # Total descriptor size (desc + signatures)
        stored_size,        # IMEM + DMEM size
        total_size,         # Total payload size
    )

    assert len(header) == HEADER_SIZE

    output = header + fwsec_blob

    with open(OUTPUT_FILE, 'wb') as f:
        f.write(output)

    print(f"Saved: {OUTPUT_FILE}")
    print(f"  Header:     {HEADER_SIZE} bytes")
    print(f"  Descriptor: {FALCON_UCODE_DESC_V3_SIZE} bytes")
    print(f"  Signatures: {desc_total_size - FALCON_UCODE_DESC_V3_SIZE} bytes")
    print(f"  IMEM:       {best_desc['imem_load_size']} bytes")
    print(f"  DMEM:       {best_desc['dmem_load_size']} bytes")
    print(f"  Total file: {len(output)} bytes")

    # Verify by dumping first few bytes of each section
    print("\n=== Verification ===")
    print("V3 Descriptor (first 44 bytes):")
    hexdump(fwsec_blob, 0, FALCON_UCODE_DESC_V3_SIZE, "  ")

    sig_start = FALCON_UCODE_DESC_V3_SIZE
    sig_size = desc_total_size - FALCON_UCODE_DESC_V3_SIZE
    if sig_size > 0:
        print(f"\nSignatures (first 32 bytes of {sig_size}):")
        hexdump(fwsec_blob, sig_start, min(32, sig_size), "  ")

    imem_start = desc_total_size
    print(f"\nIMEM (first 32 bytes of {best_desc['imem_load_size']}):")
    hexdump(fwsec_blob, imem_start, min(32, best_desc['imem_load_size']), "  ")

    dmem_start = desc_total_size + best_desc['imem_load_size']
    print(f"\nDMEM (first 32 bytes of {best_desc['dmem_load_size']}):")
    hexdump(fwsec_blob, dmem_start, min(32, best_desc['dmem_load_size']), "  ")

    print("\nDone!")

if __name__ == '__main__':
    main()
