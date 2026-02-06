#!/usr/bin/env python3
"""
NVIDIA VBIOS Analyzer for Ada Lovelace
Based on NVIDIA open-gpu-kernel-modules
"""

import struct
import sys

def read_u8(data, offset):
    return data[offset]

def read_u16(data, offset):
    return struct.unpack('<H', data[offset:offset+2])[0]

def read_u32(data, offset):
    return struct.unpack('<I', data[offset:offset+4])[0]

def find_bit_header(data):
    """Find BIT header (0xFFB8 'BIT')"""
    for i in range(0, len(data) - 12, 1):
        if data[i:i+2] == b'\xff\xb8' and data[i+2:i+6] == b'BIT\x00':
            return i
    return None

def parse_bit_tokens(data, bit_offset):
    """Parse BIT tokens"""
    header_size = read_u8(data, bit_offset + 8)
    token_size = read_u8(data, bit_offset + 9)
    token_count = read_u8(data, bit_offset + 10)

    print(f"BIT Header @ 0x{bit_offset:X}")
    print(f"  HeaderSize: {header_size}")
    print(f"  TokenSize: {token_size}")
    print(f"  TokenCount: {token_count}")
    print()

    tokens = []
    for i in range(token_count):
        tok_offset = bit_offset + header_size + i * token_size
        tok_id = read_u8(data, tok_offset)
        tok_ver = read_u8(data, tok_offset + 1)
        tok_size = read_u16(data, tok_offset + 2)
        tok_ptr = read_u16(data, tok_offset + 4)

        tokens.append({
            'id': tok_id,
            'version': tok_ver,
            'size': tok_size,
            'ptr': tok_ptr
        })

        # Token names
        names = {
            0x32: '2 (INIT)',
            0x42: 'B (BIOSDATA)',
            0x43: 'C (CLOCK)',
            0x44: 'D (DFP)',
            0x49: 'I (I2C)',
            0x4D: 'M (MEMORY)',
            0x4E: 'N (NV)',
            0x50: 'P (PERF)',
            0x53: 'S (STRING)',
            0x54: 'T (TMDS)',
            0x55: 'U (USB)',
            0x56: 'V (VIRTUAL)',
            0x70: 'p (FALCON_DATA)',
            0x75: 'u (UNK75)',
            0x78: 'x (UNK78)',
            0x64: 'd (DISPLAY)',
            0x69: 'i (UNK69)',
            0x45: 'E (UNK45)',
            0x73: 's (UNK73)',
        }
        name = names.get(tok_id, f'? (0x{tok_id:02X})')
        print(f"  Token {i:2d}: {name:20s} Ver={tok_ver} Size=0x{tok_size:04X} Ptr=0x{tok_ptr:04X}")

    return tokens

def analyze_falcon_data(data, ptr):
    """Analyze FALCON_DATA token"""
    falcon_ucode_table_ptr = read_u32(data, ptr)
    print(f"\nFALCON_DATA @ 0x{ptr:X}:")
    print(f"  FalconUcodeTablePtr: 0x{falcon_ucode_table_ptr:08X}")

    # Check if this offset is within the data
    if falcon_ucode_table_ptr < len(data):
        print(f"\n  Data @ 0x{falcon_ucode_table_ptr:X}:")
        for i in range(0, 32, 8):
            bytes_str = ' '.join(f'{data[falcon_ucode_table_ptr+i+j]:02X}' for j in range(8))
            print(f"    {bytes_str}")

        # Try to interpret as PMU table header
        ver = read_u8(data, falcon_ucode_table_ptr)
        hdr_size = read_u8(data, falcon_ucode_table_ptr + 1)
        entry_size = read_u8(data, falcon_ucode_table_ptr + 2)
        entry_count = read_u8(data, falcon_ucode_table_ptr + 3)

        print(f"\n  As PMU Header: Version={ver} HeaderSize={hdr_size} EntrySize={entry_size} EntryCount={entry_count}")

        if ver == 1 and hdr_size >= 6 and entry_size >= 6 and entry_count > 0:
            print("  Valid PMU Table V1!")
            for i in range(entry_count):
                entry_off = falcon_ucode_table_ptr + hdr_size + i * entry_size
                app_id = read_u8(data, entry_off)
                target_id = read_u8(data, entry_off + 1)
                desc_ptr = read_u32(data, entry_off + 2)
                print(f"    Entry {i}: AppID=0x{app_id:02X} TargetID=0x{target_id:02X} DescPtr=0x{desc_ptr:08X}")
        else:
            print("  NOT a valid PMU Table V1")
    else:
        print(f"  Offset 0x{falcon_ucode_table_ptr:X} is beyond data size!")

    return falcon_ucode_table_ptr

def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <vbios.rom>")
        sys.exit(1)

    with open(sys.argv[1], 'rb') as f:
        data = f.read()

    print(f"VBIOS size: {len(data)} bytes (0x{len(data):X})")
    print(f"First bytes: {data[:4].hex()}")
    print()

    # Check for NVGI header
    if data[:4] == b'NVGI':
        print("WARNING: NVGI format detected - this is compressed!")
        # Find first 55AA
        for i in range(0, len(data), 512):
            if data[i:i+2] == b'\x55\xaa':
                print(f"Found ROM signature at 0x{i:X}")
                break

    # Find BIT header
    bit_offset = find_bit_header(data)
    if bit_offset is None:
        print("BIT header not found!")
        sys.exit(1)

    tokens = parse_bit_tokens(data, bit_offset)

    # Find FALCON_DATA token (0x70)
    falcon_data_token = None
    for tok in tokens:
        if tok['id'] == 0x70:
            falcon_data_token = tok
            break

    if falcon_data_token:
        analyze_falcon_data(data, falcon_data_token['ptr'])

if __name__ == '__main__':
    main()
