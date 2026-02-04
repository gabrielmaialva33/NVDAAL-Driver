/**
 * @file test_vbios_parse.c
 * @brief Standalone test for VBIOS parsing - validates FWSEC extraction
 *        Updated for Ada Lovelace: Uses Token 0x50 instead of Token 0x70
 *
 * Compile: clang -o test_vbios_parse test_vbios_parse.c
 * Run: ./test_vbios_parse /path/to/AD102.rom
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

//
// Constants
//
#define VBIOS_ROM_SIGNATURE         0xAA55
#define VBIOS_BIT_SIGNATURE         0x00544942  // "BIT\0"
#define VBIOS_PCIR_SIGNATURE        0x52494350  // "PCIR"
#define BIT_TOKEN_PMU_TABLE         0x50        // Ada Lovelace: direct PMU table offsets
#define BIT_TOKEN_FALCON_DATA       0x70        // Pre-Ada: Falcon ucode table
#define PMU_APP_ID_FWSEC_PROD       0x85
#define PMU_APP_ID_FWSEC_DBG        0x86
#define FWSEC_DMEM_MAPPER_SIG       0x50414D44  // "DMAP"

// Ada PMU table signature
#define PMU_TABLE_VERSION_ADA       0x01
#define PMU_TABLE_HEADER_SIZE_ADA   0x06
#define PMU_TABLE_ENTRY_SIZE_ADA    0x06

#pragma pack(push, 1)

typedef struct {
    uint16_t Signature;
    uint8_t  Reserved[0x16];
    uint16_t PcirOffset;
} ROM_HEADER;

typedef struct {
    uint32_t Signature;
    uint16_t VendorId;
    uint16_t DeviceId;
    uint16_t Reserved1;
    uint16_t Length;
    uint8_t  Revision;
    uint8_t  ClassCode[3];
    uint16_t ImageLength;
    uint16_t CodeRevision;
    uint8_t  CodeType;
    uint8_t  Indicator;
    uint16_t MaxRuntimeSize;
    uint16_t ConfigUtilityPtr;
    uint16_t DmtfClpPtr;
} PCIR_HEADER;

typedef struct {
    uint16_t Prefix;
    uint8_t  Signature[4];
    uint16_t Version;
    uint8_t  HeaderSize;
    uint8_t  TokenSize;
    uint8_t  TokenCount;
    uint8_t  Reserved;
} BIT_HEADER;

typedef struct {
    uint8_t  Id;
    uint8_t  Version;
    uint16_t DataSize;
    uint16_t DataOffset;
} BIT_TOKEN;

typedef struct {
    uint32_t UcodeTableOffset;
    uint32_t UcodeTableSize;
} FALCON_DATA;

typedef struct {
    uint8_t  Version;
    uint8_t  HeaderSize;
    uint8_t  EntrySize;
    uint8_t  EntryCount;
    uint8_t  DescVersion;
    uint8_t  Reserved;
} PMU_LOOKUP_TABLE_HEADER;

// Pre-Ada format: 1-byte appId + 1-byte targetId + 4-byte offset
typedef struct {
    uint8_t  AppId;
    uint8_t  TargetId;
    uint32_t DataOffset;
} PMU_LOOKUP_TABLE_ENTRY;

// Ada format: 2-byte appId + 4-byte offset
typedef struct {
    uint16_t AppId;
    uint32_t DataOffset;
} PMU_LOOKUP_TABLE_ENTRY_ADA;

typedef struct {
    uint16_t VendorId;
    uint16_t Version;
    uint32_t Reserved;
    uint32_t TotalSize;
    uint32_t HeaderOffset;
    uint32_t HeaderSize;
    uint32_t DataOffset;
    uint32_t DataSize;
} NVFW_BIN_HDR;

typedef struct {
    NVFW_BIN_HDR BinHdr;
    uint32_t StoredSize;
    uint32_t PkcDataOffset;
    uint32_t InterfaceOffset;
    uint32_t ImemPhysBase;
    uint32_t ImemLoadSize;
    uint32_t ImemVirtBase;
    uint32_t DmemPhysBase;
    uint32_t DmemLoadSize;
    uint32_t EngineIdMask;
    uint8_t  UcodeId;
    uint8_t  SignatureCount;
    uint16_t SignatureVersions;
} FALCON_UCODE_DESC_V3;

typedef struct {
    uint32_t Signature;
    uint32_t Version;
    uint32_t Size;
    uint32_t CmdInBufferOffset;
    uint32_t CmdInBufferSize;
    uint32_t CmdOutBufferOffset;
    uint32_t CmdOutBufferSize;
    uint32_t InitCmd;
    uint32_t Features;
    uint32_t CmdMask0;
    uint32_t CmdMask1;
} FALCON_APPIF_DMEMMAPPER_V3;

#pragma pack(pop)

// Global ROM data
uint8_t *gRomData = NULL;
size_t   gRomSize = 0;
uint32_t gRomBase = 0;

//
// Helper: Read 32-bit value
//
uint32_t Read32(uint32_t offset) {
    if (offset + 4 > gRomSize) return 0xBADF5EAD;
    return *(uint32_t*)(gRomData + offset);
}

uint16_t Read16(uint32_t offset) {
    if (offset + 2 > gRomSize) return 0xDEAD;
    return *(uint16_t*)(gRomData + offset);
}

//
// Step 1: Find ROM signature
//
bool FindRomSignature(uint32_t *romBase) {
    printf("\n=== Step 1: Finding ROM Signature ===\n");

    // Try offset 0 first
    if (Read16(0) == VBIOS_ROM_SIGNATURE) {
        printf("  Found ROM signature 0xAA55 at offset 0x0\n");
        *romBase = 0;
        return true;
    }

    // Search for it
    for (uint32_t i = 0; i < gRomSize - 2; i += 0x200) {
        if (Read16(i) == VBIOS_ROM_SIGNATURE) {
            printf("  Found ROM signature 0xAA55 at offset 0x%X\n", i);
            *romBase = i;
            return true;
        }
    }

    printf("  ERROR: ROM signature not found!\n");
    return false;
}

//
// Step 2: Parse PCIR structures
//
void ParsePcirStructures(uint32_t romBase) {
    printf("\n=== Step 2: Parsing PCIR Structures ===\n");

    uint32_t offset = romBase;
    int imageCount = 0;

    while (offset < gRomSize) {
        ROM_HEADER *rom = (ROM_HEADER*)(gRomData + offset);

        if (rom->Signature != VBIOS_ROM_SIGNATURE) {
            break;
        }

        uint32_t pcirOff = offset + rom->PcirOffset;
        if (pcirOff >= gRomSize) break;

        PCIR_HEADER *pcir = (PCIR_HEADER*)(gRomData + pcirOff);

        if (pcir->Signature != VBIOS_PCIR_SIGNATURE) {
            printf("  Image %d @ 0x%X: Invalid PCIR signature\n", imageCount, offset);
            break;
        }

        printf("  Image %d @ 0x%X:\n", imageCount, offset);
        printf("    Vendor: 0x%04X, Device: 0x%04X\n", pcir->VendorId, pcir->DeviceId);
        printf("    CodeType: 0x%02X (%s)\n", pcir->CodeType,
               pcir->CodeType == 0x00 ? "x86" :
               pcir->CodeType == 0x03 ? "EFI" :
               pcir->CodeType == 0xE0 ? "FWSEC" : "Unknown");
        printf("    ImageLength: 0x%X (%d bytes)\n",
               pcir->ImageLength, pcir->ImageLength * 512);
        printf("    Last: %s\n", (pcir->Indicator & 0x80) ? "YES" : "NO");

        if (pcir->Indicator & 0x80) break;

        offset += pcir->ImageLength * 512;
        imageCount++;

        if (imageCount > 10) break; // Safety
    }
}

//
// Step 3: Find BIT header
//
bool FindBitHeader(uint32_t *bitOffset) {
    printf("\n=== Step 3: Finding BIT Header ===\n");

    // Search for BIT signature (0xB8FF followed by "BIT\0")
    for (uint32_t i = 0; i < gRomSize - 12; i++) {
        if (Read16(i) == 0xB8FF) {
            uint32_t sig = Read32(i + 2);
            if (sig == VBIOS_BIT_SIGNATURE) {
                BIT_HEADER *bit = (BIT_HEADER*)(gRomData + i);
                printf("  Found BIT header @ 0x%X\n", i);
                printf("    Version: 0x%04X\n", bit->Version);
                printf("    HeaderSize: %d\n", bit->HeaderSize);
                printf("    TokenSize: %d\n", bit->TokenSize);
                printf("    TokenCount: %d\n", bit->TokenCount);
                *bitOffset = i;
                return true;
            }
        }
    }

    printf("  ERROR: BIT header not found!\n");
    return false;
}

//
// Step 4: Find PMU Table Token (0x50 for Ada, 0x70 for pre-Ada)
//
bool FindPmuTableToken(uint32_t romBase, uint32_t bitOffset,
                       uint32_t *pmuTokenOffset, uint16_t *pmuTokenSize,
                       uint32_t *falconDataOffset) {
    printf("\n=== Step 4: Finding BIT Tokens ===\n");

    BIT_HEADER *bit = (BIT_HEADER*)(gRomData + bitOffset);
    uint32_t tokenBase = bitOffset + bit->HeaderSize;

    printf("  Scanning %d tokens starting @ 0x%X...\n", bit->TokenCount, tokenBase);
    printf("  (Token dataOffset is relative to ROM base 0x%X)\n", romBase);

    *pmuTokenOffset = 0;
    *pmuTokenSize = 0;
    *falconDataOffset = 0;

    for (int i = 0; i < bit->TokenCount; i++) {
        uint32_t tokenOff = tokenBase + (i * bit->TokenSize);
        BIT_TOKEN *token = (BIT_TOKEN*)(gRomData + tokenOff);

        printf("    Token %2d: id=0x%02X, ver=%d, size=%d, dataOff=0x%04X (abs=0x%X)\n",
               i, token->Id, token->Version, token->DataSize, token->DataOffset,
               romBase + token->DataOffset);

        if (token->Id == BIT_TOKEN_PMU_TABLE) {
            printf("  >>> Found PMU_TABLE token (0x50) - Ada Lovelace path!\n");
            *pmuTokenOffset = romBase + token->DataOffset;
            *pmuTokenSize = token->DataSize;
        } else if (token->Id == BIT_TOKEN_FALCON_DATA) {
            printf("  >>> Found FALCON_DATA token (0x70) - Pre-Ada path\n");
            *falconDataOffset = romBase + token->DataOffset;
        }
    }

    if (*pmuTokenOffset != 0) {
        printf("\n  Using Ada Lovelace Token 0x50 path\n");
        return true;
    } else if (*falconDataOffset != 0) {
        printf("\n  Using pre-Ada Token 0x70 path\n");
        return true;
    }

    printf("  ERROR: Neither PMU_TABLE nor FALCON_DATA token found!\n");
    return false;
}

//
// Step 5a: Parse Token 0x50 to find PMU Table (Ada Lovelace)
//
bool FindPmuTableViaToken50(uint32_t pmuTokenOffset, uint16_t pmuTokenSize, uint32_t *pmuTableOffset) {
    printf("\n=== Step 5a: Parsing Token 0x50 (Ada Lovelace) ===\n");

    if (pmuTokenOffset + pmuTokenSize > gRomSize) {
        printf("  ERROR: Token 0x50 data out of bounds\n");
        return false;
    }

    // Token 0x50 format for Ada: Raw array of 32-bit offsets (NO header!)
    // The first bytes are NOT version/count - they ARE the first offset
    const uint8_t *tokenData = gRomData + pmuTokenOffset;

    // Print raw bytes of token data
    printf("  Token 0x50 @ 0x%X (size=%d bytes):\n", pmuTokenOffset, pmuTokenSize);
    printf("  Raw data: ");
    for (int i = 0; i < (pmuTokenSize > 32 ? 32 : pmuTokenSize); i++) {
        printf("%02X ", tokenData[i]);
    }
    if (pmuTokenSize > 32) printf("...");
    printf("\n");

    // Parse offset array (4 bytes each, starting at byte 0 - NO header!)
    const uint32_t *offsets = (const uint32_t *)tokenData;
    int numOffsets = pmuTokenSize / 4;

    printf("  Number of offset entries: %d\n", numOffsets);
    printf("  Checking offsets for valid PMU table (signature 01 06 06):\n");

    for (int i = 0; i < numOffsets && i < 64; i++) {
        uint32_t offset = offsets[i];

        // Skip zero offsets
        if (offset == 0) continue;

        // Skip clearly invalid offsets (too large)
        if (offset >= gRomSize - sizeof(PMU_LOOKUP_TABLE_HEADER)) continue;

        PMU_LOOKUP_TABLE_HEADER *hdr = (PMU_LOOKUP_TABLE_HEADER *)(gRomData + offset);

        printf("    [%2d] 0x%08X -> v=%d h=%d e=%d c=%d",
               i, offset, hdr->Version, hdr->HeaderSize, hdr->EntrySize, hdr->EntryCount);

        // Check for Ada PMU table signature: 01 06 06 xx
        if (hdr->Version == PMU_TABLE_VERSION_ADA &&
            hdr->HeaderSize == PMU_TABLE_HEADER_SIZE_ADA &&
            hdr->EntrySize == PMU_TABLE_ENTRY_SIZE_ADA &&
            hdr->EntryCount > 0 && hdr->EntryCount <= 32) {
            printf(" *** VALID PMU TABLE! ***");
            *pmuTableOffset = offset;
        }
        printf("\n");
    }

    if (*pmuTableOffset != 0) {
        printf("\n  Found valid PMU table @ 0x%X\n", *pmuTableOffset);
        return true;
    }

    printf("  ERROR: No valid PMU table found in Token 0x50 offsets\n");
    return false;
}

//
// Step 5b: Parse Token 0x70 to find PMU Table (Pre-Ada fallback)
//
bool FindPmuTableViaToken70(uint32_t romBase, uint32_t falconDataOffset, uint32_t *pmuTableOffset) {
    printf("\n=== Step 5b: Parsing Token 0x70 (Pre-Ada Fallback) ===\n");

    FALCON_DATA *fd = (FALCON_DATA*)(gRomData + falconDataOffset);

    printf("  FALCON_DATA @ 0x%X:\n", falconDataOffset);
    printf("    UcodeTableOffset (raw): 0x%08X\n", fd->UcodeTableOffset);

    // Try ROM-relative interpretation
    uint32_t pmuOff = romBase + fd->UcodeTableOffset;
    if (pmuOff < gRomSize) {
        PMU_LOOKUP_TABLE_HEADER *hdr = (PMU_LOOKUP_TABLE_HEADER *)(gRomData + pmuOff);
        printf("  Trying ROM-relative @ 0x%X: v=%d h=%d e=%d c=%d\n",
               pmuOff, hdr->Version, hdr->HeaderSize, hdr->EntrySize, hdr->EntryCount);

        if (hdr->Version >= 1 && hdr->Version <= 10 &&
            hdr->HeaderSize >= 4 && hdr->EntrySize >= 4 &&
            hdr->EntryCount > 0 && hdr->EntryCount <= 50) {
            *pmuTableOffset = pmuOff;
            return true;
        }
    }

    printf("  Token 0x70 path failed - searching by pattern...\n");
    return false;
}

//
// Step 6: Find FWSEC entry in PMU table
//
bool FindFwsecEntry(uint32_t pmuTableOffset, uint32_t *fwsecDescOffset) {
    printf("\n=== Step 6: Finding FWSEC Entry ===\n");

    PMU_LOOKUP_TABLE_HEADER *pmu = (PMU_LOOKUP_TABLE_HEADER*)(gRomData + pmuTableOffset);
    uint32_t entryBase = pmuTableOffset + pmu->HeaderSize;

    printf("  PMU Table @ 0x%X:\n", pmuTableOffset);
    printf("    Version: %d, HeaderSize: %d, EntrySize: %d, EntryCount: %d\n",
           pmu->Version, pmu->HeaderSize, pmu->EntrySize, pmu->EntryCount);

    // 6-byte entries can be EITHER format - check both!
    // Pre-Ada: appId(1) + targetId(1) + offset(4) = 6 bytes
    // Ada: appId(2) + offset(4) = 6 bytes
    printf("  Note: 6-byte entries - will check both formats\n");

    printf("\n  Scanning %d PMU entries starting @ 0x%X...\n", pmu->EntryCount, entryBase);

    for (int i = 0; i < pmu->EntryCount; i++) {
        uint32_t entryOff = entryBase + (i * pmu->EntrySize);

        // Parse as pre-Ada format (1-byte appId)
        PMU_LOOKUP_TABLE_ENTRY *entry = (PMU_LOOKUP_TABLE_ENTRY*)(gRomData + entryOff);
        uint8_t appId8 = entry->AppId;
        uint8_t targetId = entry->TargetId;
        uint32_t dataOffset = entry->DataOffset;

        printf("    Entry %2d: AppId=0x%02X, TargetId=0x%02X, DataOffset=0x%08X\n",
               i, appId8, targetId, dataOffset);

        // Check pre-Ada format (1-byte appId)
        if (appId8 == PMU_APP_ID_FWSEC_PROD || appId8 == PMU_APP_ID_FWSEC_DBG) {
            printf("  >>> Found FWSEC entry (pre-Ada format)! AppId=0x%02X\n", appId8);
            *fwsecDescOffset = dataOffset;
            return true;
        }

        // Also check Ada format (2-byte appId) - but this changes offset interpretation
        PMU_LOOKUP_TABLE_ENTRY_ADA *entryAda = (PMU_LOOKUP_TABLE_ENTRY_ADA*)(gRomData + entryOff);
        uint16_t appId16 = entryAda->AppId;
        if (appId16 == 0x0085 || appId16 == 0x0086) {
            printf("  >>> Found FWSEC entry (Ada format)! AppId=0x%04X\n", appId16);
            *fwsecDescOffset = entryAda->DataOffset;
            return true;
        }
    }

    printf("  ERROR: FWSEC entry not found in PMU table!\n");
    return false;
}

//
// Step 7: Parse FWSEC descriptor
//
bool ParseFwsecDescriptor(uint32_t fwsecDescOffset) {
    printf("\n=== Step 7: Parsing FWSEC Descriptor ===\n");

    printf("  FWSEC DataOffset from PMU entry: 0x%X\n", fwsecDescOffset);

    // For Ada, the offset is typically absolute in ROM
    uint32_t candidates[] = {
        fwsecDescOffset,                   // Direct offset
        gRomBase + fwsecDescOffset,        // ROM-relative
    };
    const char *names[] = {"direct", "ROM-relative"};

    uint32_t validOffset = 0;
    bool found = false;

    for (int c = 0; c < 2 && !found; c++) {
        uint32_t offset = candidates[c];
        if (offset >= gRomSize - sizeof(FALCON_UCODE_DESC_V3)) continue;

        FALCON_UCODE_DESC_V3 *desc = (FALCON_UCODE_DESC_V3*)(gRomData + offset);

        printf("\n  Trying %s @ 0x%X:\n", names[c], offset);
        printf("    VendorId: 0x%04X %s\n", desc->BinHdr.VendorId,
               desc->BinHdr.VendorId == 0x10DE ? "(NVIDIA - VALID!)" : "");

        if (desc->BinHdr.VendorId == 0x10DE) {
            validOffset = offset;
            found = true;
        }
    }

    if (!found) {
        printf("\n  Standard offsets failed. Searching for NVIDIA vendor ID...\n");

        // Search for 0x10DE near the expected location
        uint32_t searchStart = fwsecDescOffset > 0x10000 ? fwsecDescOffset - 0x1000 : 0;
        uint32_t searchEnd = fwsecDescOffset + 0x10000;
        if (searchEnd > gRomSize) searchEnd = gRomSize;

        for (uint32_t i = searchStart; i < searchEnd - sizeof(FALCON_UCODE_DESC_V3); i += 4) {
            uint16_t vid = *(uint16_t*)(gRomData + i);
            if (vid == 0x10DE) {
                uint16_t ver = *(uint16_t*)(gRomData + i + 2);
                if (ver >= 1 && ver <= 0x10) {
                    FALCON_UCODE_DESC_V3 *desc = (FALCON_UCODE_DESC_V3*)(gRomData + i);
                    if (desc->BinHdr.TotalSize > 0 && desc->BinHdr.TotalSize < 0x100000) {
                        printf("  Found NVIDIA descriptor @ 0x%X\n", i);
                        validOffset = i;
                        found = true;
                        break;
                    }
                }
            }
        }
    }

    if (!found) {
        printf("  ERROR: Could not find valid FWSEC descriptor!\n");
        return false;
    }

    FALCON_UCODE_DESC_V3 *desc = (FALCON_UCODE_DESC_V3*)(gRomData + validOffset);

    printf("\n  FWSEC Descriptor @ 0x%X:\n", validOffset);
    printf("    BinHdr.VendorId: 0x%04X (NVIDIA)\n", desc->BinHdr.VendorId);
    printf("    BinHdr.Version: 0x%04X\n", desc->BinHdr.Version);
    printf("    BinHdr.TotalSize: 0x%X (%d bytes)\n", desc->BinHdr.TotalSize, desc->BinHdr.TotalSize);
    printf("    BinHdr.HeaderOffset: 0x%X\n", desc->BinHdr.HeaderOffset);
    printf("    BinHdr.DataOffset: 0x%X\n", desc->BinHdr.DataOffset);
    printf("    BinHdr.DataSize: 0x%X (%d bytes)\n", desc->BinHdr.DataSize, desc->BinHdr.DataSize);
    printf("\n");
    printf("    StoredSize: 0x%X (%d bytes)\n", desc->StoredSize, desc->StoredSize);
    printf("    InterfaceOffset: 0x%X\n", desc->InterfaceOffset);
    printf("    ImemPhysBase: 0x%X\n", desc->ImemPhysBase);
    printf("    ImemLoadSize: 0x%X (%d bytes)\n", desc->ImemLoadSize, desc->ImemLoadSize);
    printf("    DmemPhysBase: 0x%X\n", desc->DmemPhysBase);
    printf("    DmemLoadSize: 0x%X (%d bytes)\n", desc->DmemLoadSize, desc->DmemLoadSize);
    printf("    UcodeId: 0x%02X\n", desc->UcodeId);
    printf("    SignatureCount: %d\n", desc->SignatureCount);

    // Check for DMEM Mapper (DMAP)
    if (desc->InterfaceOffset > 0 && desc->InterfaceOffset < desc->DmemLoadSize) {
        uint32_t dmemStart = validOffset + desc->BinHdr.DataOffset + desc->DmemPhysBase;
        uint32_t dmapOffset = dmemStart + desc->InterfaceOffset;

        if (dmapOffset + sizeof(FALCON_APPIF_DMEMMAPPER_V3) < gRomSize) {
            uint32_t dmapSig = Read32(dmapOffset);
            printf("\n  DMEM Mapper @ 0x%X:\n", dmapOffset);
            printf("    Signature: 0x%08X %s\n", dmapSig,
                   dmapSig == FWSEC_DMEM_MAPPER_SIG ? "(DMAP OK!)" : "(not DMAP)");

            if (dmapSig == FWSEC_DMEM_MAPPER_SIG) {
                FALCON_APPIF_DMEMMAPPER_V3 *dmap = (FALCON_APPIF_DMEMMAPPER_V3*)(gRomData + dmapOffset);
                printf("    InitCmd: 0x%02X (need 0x15 for FRTS)\n", dmap->InitCmd);
                printf("    CmdMask0: 0x%08X (FRTS bit=%d)\n",
                       dmap->CmdMask0, (dmap->CmdMask0 >> 0x15) & 1);
            }
        }
    }

    return true;
}

//
// Fallback: Search for PMU table by pattern
//
bool SearchPmuTableByPattern(uint32_t *pmuTableOffset) {
    printf("\n=== Fallback: Searching PMU Table by Pattern ===\n");

    for (uint32_t i = 0x9000; i < gRomSize - 32; i += 4) {
        PMU_LOOKUP_TABLE_HEADER *pmu = (PMU_LOOKUP_TABLE_HEADER*)(gRomData + i);

        // Check for PMU table signature: version=1, headerSize=6, entrySize=6
        if (pmu->Version == 1 && pmu->HeaderSize == 6 && pmu->EntrySize == 6 &&
            pmu->EntryCount > 0 && pmu->EntryCount <= 32) {

            // Verify it has FWSEC entry (check BOTH formats since entry size is same)
            uint32_t entryBase = i + pmu->HeaderSize;
            for (int j = 0; j < pmu->EntryCount; j++) {
                uint32_t entryOff = entryBase + j * pmu->EntrySize;

                // Check pre-Ada format (1-byte appId at byte 0)
                uint8_t appId8 = gRomData[entryOff];
                if (appId8 == 0x85 || appId8 == 0x86) {
                    printf("  Found PMU table @ 0x%X with FWSEC entry (appId=0x%02X)!\n", i, appId8);
                    *pmuTableOffset = i;
                    return true;
                }

                // Check Ada format (2-byte appId)
                uint16_t appId16 = *(uint16_t*)(gRomData + entryOff);
                if (appId16 == 0x0085 || appId16 == 0x0086) {
                    printf("  Found PMU table @ 0x%X with FWSEC entry (appId=0x%04X)!\n", i, appId16);
                    *pmuTableOffset = i;
                    return true;
                }
            }
        }
    }

    printf("  ERROR: Could not find PMU table by pattern search\n");
    return false;
}

//
// Main
//
int main(int argc, char *argv[]) {
    printf("====================================================\n");
    printf("  VBIOS Parser Test for Ada Lovelace FWSEC\n");
    printf("  (Updated: Token 0x50 support for Ada Lovelace)\n");
    printf("====================================================\n");

    if (argc < 2) {
        printf("Usage: %s <vbios.rom>\n", argv[0]);
        return 1;
    }

    // Load ROM file
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        printf("ERROR: Cannot open %s\n", argv[1]);
        return 1;
    }

    fseek(f, 0, SEEK_END);
    gRomSize = ftell(f);
    fseek(f, 0, SEEK_SET);

    gRomData = malloc(gRomSize);
    if (!gRomData) {
        printf("ERROR: Cannot allocate %zu bytes\n", gRomSize);
        fclose(f);
        return 1;
    }

    fread(gRomData, 1, gRomSize, f);
    fclose(f);

    printf("\nLoaded %s (%zu bytes / %zu KB)\n", argv[1], gRomSize, gRomSize / 1024);

    // Run tests
    uint32_t romBase = 0;
    uint32_t bitOffset = 0;
    uint32_t pmuTokenOffset = 0;
    uint16_t pmuTokenSize = 0;
    uint32_t falconDataOffset = 0;
    uint32_t pmuTableOffset = 0;
    uint32_t fwsecDescOffset = 0;

    bool ok = true;

    ok = ok && FindRomSignature(&romBase);
    gRomBase = romBase;

    if (ok) ParsePcirStructures(romBase);
    ok = ok && FindBitHeader(&bitOffset);
    ok = ok && FindPmuTableToken(romBase, bitOffset, &pmuTokenOffset, &pmuTokenSize, &falconDataOffset);

    // Try Token 0x50 first (Ada Lovelace)
    if (ok && pmuTokenOffset != 0) {
        // Don't fail if Token 0x50 doesn't have PMU table - just continue to fallback
        FindPmuTableViaToken50(pmuTokenOffset, pmuTokenSize, &pmuTableOffset);
    }

    // Fall back to Token 0x70 (Pre-Ada)
    if (ok && pmuTableOffset == 0 && falconDataOffset != 0) {
        FindPmuTableViaToken70(romBase, falconDataOffset, &pmuTableOffset);
    }

    // Last resort: pattern search (primary method for Ada Lovelace)
    if (ok && pmuTableOffset == 0) {
        ok = SearchPmuTableByPattern(&pmuTableOffset);
    }

    ok = ok && FindFwsecEntry(pmuTableOffset, &fwsecDescOffset);
    ok = ok && ParseFwsecDescriptor(fwsecDescOffset);

    printf("\n====================================================\n");
    if (ok) {
        printf("  SUCCESS: FWSEC extraction path validated!\n");
        printf("  \n");
        printf("  Key offsets:\n");
        printf("    ROM Base: 0x%X\n", romBase);
        printf("    BIT Header: 0x%X\n", bitOffset);
        printf("    Token 0x50 Data: 0x%X (size=%d)\n", pmuTokenOffset, pmuTokenSize);
        printf("    PMU Table: 0x%X\n", pmuTableOffset);
        printf("    FWSEC DataOffset: 0x%X\n", fwsecDescOffset);
    } else {
        printf("  FAILED: Could not complete FWSEC extraction\n");
    }
    printf("====================================================\n");

    free(gRomData);
    return ok ? 0 : 1;
}
