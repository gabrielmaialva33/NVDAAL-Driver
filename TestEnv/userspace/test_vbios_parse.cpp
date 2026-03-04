/*
 * test_vbios_parse.cpp - VBIOS ROM parser test
 *
 * Tests the VBIOS parsing logic that will be used in the NVDAAL kext.
 * Validates: ROM images, BIT header, Falcon Data, FWSEC descriptor, DMEMMAPPER.
 *
 * Usage: ./test_vbios_parse <path_to_vbios.rom>
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <cassert>
#include "NVDAALRegs.h"

static uint8_t* romData = nullptr;
static size_t romSize = 0;
static int testsPassed = 0;
static int testsFailed = 0;

#define TEST(name, cond) do { \
    if (cond) { \
        printf("  [PASS] %s\n", name); \
        testsPassed++; \
    } else { \
        printf("  [FAIL] %s\n", name); \
        testsFailed++; \
    } \
} while(0)

// Read helpers
static uint8_t  rd8(size_t off)  { return (off < romSize) ? romData[off] : 0; }
static uint16_t rd16(size_t off) { return (off+1 < romSize) ? *(uint16_t*)(romData+off) : 0; }
static uint32_t rd32(size_t off) { return (off+3 < romSize) ? *(uint32_t*)(romData+off) : 0; }

// ========================================================================
// Test 1: PCI Expansion ROM parsing
// ========================================================================
struct RomImage {
    uint32_t offset;
    uint32_t size;
    uint16_t vendorId;
    uint16_t deviceId;
    uint8_t  codeType;
    bool     isLast;
};

std::vector<RomImage> parseRomImages() {
    std::vector<RomImage> images;
    uint32_t offset = 0;

    while (offset < romSize) {
        uint16_t sig = rd16(offset);
        if (sig != 0xAA55) break;

        uint16_t pcirPtr = rd16(offset + 0x18);
        uint32_t pcirAbs = offset + pcirPtr;

        if (pcirAbs + 0x18 > romSize) break;
        if (rd32(pcirAbs) != 0x52494350) break; // "PCIR"

        RomImage img;
        img.offset   = offset;
        img.vendorId = rd16(pcirAbs + 4);
        img.deviceId = rd16(pcirAbs + 6);
        img.size     = (uint32_t)rd16(pcirAbs + 0x10) * 512;
        img.codeType = rd8(pcirAbs + 0x14);
        img.isLast   = (rd8(pcirAbs + 0x15) & 0x80) != 0;

        images.push_back(img);
        offset += img.size;
        if (img.isLast) break;
    }
    return images;
}

void testRomImages() {
    printf("\n=== Test 1: PCI Expansion ROM ===\n");
    auto images = parseRomImages();

    TEST("Found at least 2 ROM images", images.size() >= 2);
    TEST("First image is PCI/AT (0x00)", images[0].codeType == 0x00);
    TEST("Second image is EFI (0x03)", images[1].codeType == 0x03);
    TEST("Vendor is NVIDIA (0x10DE)", images[0].vendorId == 0x10DE);
    TEST("Device is RTX 4090 (0x2684)", images[0].deviceId == 0x2684);

    for (auto& img : images) {
        const char* typeStr = "???";
        if (img.codeType == 0x00) typeStr = "PCI/AT";
        else if (img.codeType == 0x03) typeStr = "EFI";
        else if (img.codeType == 0xE0) typeStr = "FWSEC";
        printf("    Image: type=%s vendor=0x%04X device=0x%04X offset=0x%06X size=%u last=%d\n",
               typeStr, img.vendorId, img.deviceId, img.offset, img.size, img.isLast);
    }
}

// ========================================================================
// Test 2: BIT Header parsing
// ========================================================================
struct BitTokenEntry {
    uint8_t  id;
    uint8_t  dataVersion;
    uint16_t dataSize;
    uint16_t dataOffset;
};

uint32_t bitOffset = 0;
std::vector<BitTokenEntry> bitTokens;

bool findBitHeader() {
    for (size_t i = 0; i + 6 < romSize; i++) {
        if (rd16(i) == BIT_HEADER_ID && rd32(i + 2) == BIT_HEADER_SIGNATURE) {
            bitOffset = (uint32_t)i;
            return true;
        }
    }
    return false;
}

void parseBitTokens() {
    BitHeader* hdr = (BitHeader*)(romData + bitOffset);
    uint32_t tokStart = bitOffset + hdr->headerSize;

    for (int t = 0; t < hdr->tokenCount; t++) {
        uint32_t tOff = tokStart + t * hdr->tokenSize;
        if (tOff + 6 > romSize) break;

        BitTokenEntry tok;
        tok.id          = rd8(tOff);
        tok.dataVersion = rd8(tOff + 1);
        tok.dataSize    = rd16(tOff + 2);
        tok.dataOffset  = rd16(tOff + 4);
        bitTokens.push_back(tok);
    }
}

void testBitHeader() {
    printf("\n=== Test 2: BIT Header ===\n");
    bool found = findBitHeader();
    TEST("BIT header found", found);
    if (!found) return;

    BitHeader* hdr = (BitHeader*)(romData + bitOffset);
    TEST("BIT ID = 0xB8FF", hdr->id == BIT_HEADER_ID);
    TEST("BIT signature = 'BIT\\0'", hdr->signature == BIT_HEADER_SIGNATURE);
    TEST("Token size = 6", hdr->tokenSize == 6);
    TEST("Token count > 0", hdr->tokenCount > 0);

    parseBitTokens();
    printf("    BIT at offset 0x%06X, %d tokens\n", bitOffset, hdr->tokenCount);

    // Check for critical tokens
    bool hasFalconData = false, hasPmuTable = false;
    for (auto& tok : bitTokens) {
        if (tok.id == BIT_TOKEN_FALCON_DATA) hasFalconData = true;
        if (tok.id == BIT_TOKEN_PMU_TABLE)   hasPmuTable = true;
    }
    TEST("Has Falcon Data token (0x70)", hasFalconData);
    TEST("Has PMU Table token (0x50)", hasPmuTable);
}

// ========================================================================
// Test 3: Falcon Ucode Table & FWSEC (Ada Lovelace DMAP-based discovery)
// ========================================================================

// Info about a discovered DMAP structure
struct DmapInfo {
    uint32_t offset;        // ROM offset of the DMAP signature
    uint16_t version;
    uint16_t size;
    uint32_t cmdBufOffset;
    uint32_t interfaceOffset;
    bool     isFrts;        // true=FWSEC-FRTS, false=FWSEC-SB
};

// Scan ROM for all DMAP signatures
std::vector<DmapInfo> scanForDmaps() {
    std::vector<DmapInfo> dmaps;
    // DMAP is 64 bytes total: we need at least 64 bytes from the signature
    for (size_t i = 0; i + 64 <= romSize; i += 4) {
        if (rd32(i) == DMEMMAPPER_SIGNATURE) {
            DmapInfo info;
            info.offset          = (uint32_t)i;
            info.version         = rd16(i + 4);
            info.size            = rd16(i + 6);
            info.cmdBufOffset    = rd32(i + 8);
            // interfaceOffset is at DMAP+0x3C (the last uint32 in the 64-byte struct)
            info.interfaceOffset = rd32(i + 0x3C);
            // Classify by cmdBufOffset: FRTS uses 0xD40, SB uses 0x26D0
            info.isFrts          = (info.cmdBufOffset == 0x0D40);
            dmaps.push_back(info);
        }
    }
    return dmaps;
}

std::vector<DmapInfo> globalDmaps;

void testFalconData() {
    printf("\n=== Test 3: Falcon Ucode Table & FWSEC ===\n");

    // Find token 0x70 and note that on Ada it does NOT contain a standard table
    BitTokenEntry* falconTok = nullptr;
    for (auto& tok : bitTokens) {
        if (tok.id == BIT_TOKEN_FALCON_DATA) { falconTok = &tok; break; }
    }
    if (!falconTok) { printf("  [SKIP] No Falcon Data token\n"); return; }

    uint32_t ucodeTableOff = rd32(falconTok->dataOffset);
    printf("    BIT Token 0x70 -> ucodeTableOffset = 0x%08X\n", ucodeTableOff);
    TEST("ucodeTableOffset within ROM", ucodeTableOff + 8 < romSize);

    // On Ada Lovelace, Token 0x70 points to falcon engine register/PLL data,
    // NOT a standard PMU lookup table. This is expected behavior.
    printf("    [INFO] Ada Lovelace: Token 0x70 contains engine data, not a lookup table.\n");
    printf("    [INFO] Using DMAP signature scanning for FWSEC discovery instead.\n");

    // Scan for DMAP signatures in the NV data region
    globalDmaps = scanForDmaps();
    printf("    Found %zu DMAP signatures in ROM\n", globalDmaps.size());
    TEST("Found DMAP signatures (>= 4 expected)", globalDmaps.size() >= 4);

    // Validate each DMAP
    int validDmaps = 0;
    int frtsDmaps = 0;
    int sbDmaps = 0;
    for (size_t d = 0; d < globalDmaps.size(); d++) {
        auto& dmap = globalDmaps[d];
        const char* typeStr = dmap.isFrts ? "FWSEC-FRTS" : "FWSEC-SB";
        printf("    DMAP[%zu] at 0x%06X: ver=%u size=%u cmdBuf=0x%04X iface=0x%04X [%s]\n",
               d, dmap.offset, dmap.version, dmap.size,
               dmap.cmdBufOffset, dmap.interfaceOffset, typeStr);

        if (dmap.version == 3 && dmap.size == 64) {
            validDmaps++;
            if (dmap.isFrts) frtsDmaps++;
            else sbDmaps++;
        }
    }

    TEST("All DMAPs have version=3 and size=64", validDmaps == (int)globalDmaps.size());
    TEST("Found FWSEC-FRTS DMAPs (cmdBuf=0xD40)", frtsDmaps >= 1);
    TEST("Found FWSEC-SB DMAPs (cmdBuf=0x26D0)", sbDmaps >= 1);
}

// ========================================================================
// Test 4: Ada Lovelace FWSEC Descriptor Discovery
// ========================================================================

// Info about a discovered Ada Falcon descriptor
struct AdaDescInfo {
    uint32_t preHeaderOff;     // ROM offset of pre-header
    uint8_t  version;          // Pre-header version (expect 1)
    uint16_t typeFlags;        // Pre-header type flags (expect 0x0010)
    uint16_t dataSize;         // Pre-header data size
    uint16_t headerSize;       // Pre-header header size (expect 0x0020)
    uint16_t descVersion;      // Descriptor body version (expect 3)
    uint16_t deviceId;         // Device ID (expect 0x2684)
    uint8_t  sigVersions;      // Packed info: signature versions bitmask
    uint8_t  ucodeId;          // Packed info: ucode ID (expect 0x04)
    uint8_t  engineIdMask;     // Packed info: engine ID mask (expect 0x08 = GSP)
    uint8_t  signatureCount;   // Packed info: number of RSA-3K sigs (expect 2)
    uint32_t codeSize;         // Size field: IMEM code size
    uint32_t interfaceOffset;  // Size field: interface offset (DMAP pos in DMEM)
    uint32_t dmapOffset;       // ROM offset of associated DMAP
    bool     isFrts;           // Classification: true=FRTS, false=SB
};

// Search backwards from a DMAP offset for the Ada descriptor pre-header pattern.
// Pattern: 01 00 10 00 XX XX 20 00 (version=1, reserved=0, type=0x0010, XX, headerSize=0x0020)
// Maximum search range: 4096 bytes before DMAP (descriptors are close to their DMAPs).
bool findAdaDescriptor(uint32_t dmapOff, AdaDescInfo& info) {
    // The descriptor is somewhere before the DMAP. The distance varies:
    // FWSEC-FRTS: desc at ~0x0430BC, DMAP at 0x043B9C => distance ~0x0AE0
    // FWSEC-SB:   desc at ~0x0699B0, DMAP at 0x06A37C => distance ~0x09CC
    // Search up to 8KB backwards (generous margin)
    uint32_t searchStart = (dmapOff > 8192) ? dmapOff - 8192 : 0;

    for (uint32_t off = searchStart; off + 8 <= dmapOff; off++) {
        // Check pre-header pattern: version=0x01, reserved=0x00, typeFlags=0x0010, headerSize=0x0020
        if (rd8(off) == ADA_FALCON_DESC_VERSION &&
            rd8(off + 1) == 0x00 &&
            rd16(off + 2) == ADA_FALCON_DESC_TYPE_FWSEC &&
            rd16(off + 6) == ADA_FALCON_DESC_HEADER_SIZE) {

            // Verify descriptor body follows: version=3, deviceId=0x2684
            uint32_t bodyOff = off + 8;
            if (bodyOff + 4 > romSize) continue;
            uint16_t descVer = rd16(bodyOff);
            uint16_t devId   = rd16(bodyOff + 2);

            if (descVer != 3 || devId != 0x2684) continue;

            // Found a valid descriptor. Now find packed info.
            // The packed info is at a variable offset after the body start.
            // For FWSEC-FRTS: 16 zero bytes between body header and packed info
            // For FWSEC-SB: 4 zero bytes between body header and packed info
            // We scan for the packed info pattern: sigVer(!=0) ucodeId(0x04) engineMask(0x08) sigCount(!=0)
            AdaFalconDescPackedInfo* packedInfo = nullptr;
            uint32_t packedInfoOff = 0;
            for (uint32_t pOff = bodyOff + 4; pOff + sizeof(AdaFalconDescPackedInfo) + sizeof(AdaFalconDescSizeFields) <= dmapOff; pOff++) {
                uint8_t p0 = rd8(pOff);
                uint8_t p1 = rd8(pOff + 1);
                uint8_t p2 = rd8(pOff + 2);
                uint8_t p3 = rd8(pOff + 3);
                // Match: signatureVersions != 0, ucodeId == 0x04, engineIdMask == 0x08, signatureCount > 0
                if (p0 != 0 && p1 == 0x04 && p2 == 0x08 && p3 > 0) {
                    packedInfo = (AdaFalconDescPackedInfo*)(romData + pOff);
                    packedInfoOff = pOff;
                    break;
                }
            }

            if (!packedInfo) continue;

            // Read size fields immediately after packed info
            uint32_t sizeOff = packedInfoOff + sizeof(AdaFalconDescPackedInfo);
            if (sizeOff + sizeof(AdaFalconDescSizeFields) > romSize) continue;
            AdaFalconDescSizeFields* sizeFields = (AdaFalconDescSizeFields*)(romData + sizeOff);

            // Fill in the info struct
            info.preHeaderOff   = off;
            info.version        = rd8(off);
            info.typeFlags      = rd16(off + 2);
            info.dataSize       = rd16(off + 4);
            info.headerSize     = rd16(off + 6);
            info.descVersion    = descVer;
            info.deviceId       = devId;
            info.sigVersions    = packedInfo->signatureVersions;
            info.ucodeId        = packedInfo->ucodeId;
            info.engineIdMask   = packedInfo->engineIdMask;
            info.signatureCount = packedInfo->signatureCount;
            info.codeSize       = sizeFields->codeSize;
            info.interfaceOffset = sizeFields->interfaceOffset;
            info.dmapOffset     = dmapOff;

            // Classify based on DMAP cmdBufOffset (already determined in DmapInfo)
            // or by interfaceOffset: FRTS=0xD2C, SB=0xC18
            info.isFrts = (sizeFields->interfaceOffset == FWSEC_FRTS_INTERFACE_OFFSET);

            return true;
        }
    }
    return false;
}

void testAdaFwsecDiscovery() {
    printf("\n=== Test 4: Ada FWSEC Descriptor Discovery ===\n");

    if (globalDmaps.empty()) {
        printf("  [SKIP] No DMAPs found (run testFalconData first)\n");
        return;
    }

    std::vector<AdaDescInfo> descs;
    int frtsCount = 0;
    int sbCount = 0;

    for (size_t d = 0; d < globalDmaps.size(); d++) {
        AdaDescInfo desc;
        bool found = findAdaDescriptor(globalDmaps[d].offset, desc);
        if (found) {
            descs.push_back(desc);
            const char* typeStr = desc.isFrts ? "FWSEC-FRTS" : "FWSEC-SB";
            if (desc.isFrts) frtsCount++;
            else sbCount++;

            printf("\n    Descriptor #%zu at 0x%06X [%s]:\n", descs.size(), desc.preHeaderOff, typeStr);
            printf("      Pre-header: version=%u typeFlags=0x%04X dataSize=0x%04X headerSize=0x%04X\n",
                   desc.version, desc.typeFlags, desc.dataSize, desc.headerSize);
            printf("      Body:       version=%u deviceId=0x%04X\n",
                   desc.descVersion, desc.deviceId);
            printf("      PackedInfo: sigVersions=%u ucodeId=0x%02X engineMask=0x%02X sigCount=%u\n",
                   desc.sigVersions, desc.ucodeId, desc.engineIdMask, desc.signatureCount);
            printf("      SizeFields: codeSize=0x%04X interfaceOffset=0x%04X\n",
                   desc.codeSize, desc.interfaceOffset);
            printf("      DMAP at:    0x%06X\n", desc.dmapOffset);

            // Verify RSA-3K signatures exist after packed info + size fields
            uint32_t sigsStart = desc.preHeaderOff + 8 + desc.headerSize;
            // For FWSEC-FRTS: sigs at preHeader+48 (8 pre + 4 body + 16 pad + 4 packed + 16 size = 48)
            // For FWSEC-SB: sigs at preHeader+36 (8 pre + 4 body + 4 pad + 4 packed + 16 size = 36)
            // But actually, signatures are at known offsets from the analysis:
            // desc+0x30 for FRTS (#1: 0x0430BC+0x30=0x0430EC), desc+0x24 for SB (#3: 0x0699B0+0x24=0x0699D4)
            uint32_t sigsTotalSize = (uint32_t)desc.signatureCount * BCRT30_RSA3K_SIG_SIZE;

            // Verify first signature bytes are non-zero (at least one of the first 16 bytes)
            bool sigDataPresent = false;
            for (uint32_t b = 0; b < 16 && sigsStart + b < romSize; b++) {
                if (rd8(sigsStart + b) != 0x00) {
                    sigDataPresent = true;
                    break;
                }
            }
            // SB signatures start with zeros (RSA padding), check deeper
            if (!sigDataPresent) {
                for (uint32_t b = 0; b < sigsTotalSize && sigsStart + b < romSize; b++) {
                    if (rd8(sigsStart + b) != 0x00) {
                        sigDataPresent = true;
                        break;
                    }
                }
            }

            printf("      Signatures: %u x %u bytes = %u bytes starting at 0x%06X %s\n",
                   desc.signatureCount, BCRT30_RSA3K_SIG_SIZE, sigsTotalSize,
                   sigsStart, sigDataPresent ? "(data present)" : "(EMPTY!)");
        } else {
            printf("    [WARN] No Ada descriptor found before DMAP at 0x%06X\n", globalDmaps[d].offset);
        }
    }

    printf("\n");
    TEST("Found Ada FWSEC descriptors", descs.size() >= 4);
    TEST("Found FWSEC-FRTS descriptors (>= 2)", frtsCount >= 2);
    TEST("Found FWSEC-SB descriptors (>= 2)", sbCount >= 2);

    // Validate first descriptor in detail (should be FWSEC-FRTS primary)
    if (!descs.empty()) {
        auto& first = descs[0];
        TEST("First desc: pre-header version = 1", first.version == ADA_FALCON_DESC_VERSION);
        TEST("First desc: type flags = 0x0010", first.typeFlags == ADA_FALCON_DESC_TYPE_FWSEC);
        TEST("First desc: header size = 0x0020", first.headerSize == ADA_FALCON_DESC_HEADER_SIZE);
        TEST("First desc: body version = 3", first.descVersion == 3);
        TEST("First desc: deviceId = 0x2684 (RTX 4090)", first.deviceId == 0x2684);
        TEST("First desc: ucodeId = 0x04 (GSP/FWSEC)", first.ucodeId == 0x04);
        TEST("First desc: engineIdMask = 0x08 (GSP Falcon)", first.engineIdMask == 0x08);
        TEST("First desc: signatureCount = 2", first.signatureCount == 2);
    }
}

// ========================================================================
// Test 5: GspSystemInfo struct layout validation
// ========================================================================
void testStructLayouts() {
    printf("\n=== Test 5: Struct Layout Validation ===\n");

    // These sizes must match NVIDIA's open-gpu-kernel-modules exactly
    TEST("NvRpcMessageHeader size = 24", sizeof(NvRpcMessageHeader) == 24);
    TEST("GspQueueElement has flexible array", sizeof(GspQueueElement) == 16);
    TEST("GspFwWprMeta size > 100", sizeof(GspFwWprMeta) > 100);
    TEST("FalconUcodeDescV3Nvidia size = 44", sizeof(FalconUcodeDescV3Nvidia) == 44);

    printf("    NvRpcMessageHeader: %zu bytes\n", sizeof(NvRpcMessageHeader));
    printf("    GspQueueElement:    %zu bytes (header only)\n", sizeof(GspQueueElement));
    printf("    GspFwWprMeta:       %zu bytes\n", sizeof(GspFwWprMeta));
    printf("    GspLibosInitArgs:   %zu bytes\n", sizeof(GspLibosInitArgs));
    printf("    GspSystemInfo:      %zu bytes\n", sizeof(GspSystemInfo));
    printf("    FalconUcodeDescV3:  %zu bytes\n", sizeof(FalconUcodeDescV3Nvidia));
    printf("    DmemMapperHeader:   %zu bytes\n", sizeof(DmemMapperHeader));
    printf("    BitHeader:          %zu bytes\n", sizeof(BitHeader));
    printf("    BitToken:           %zu bytes\n", sizeof(BitToken));
    printf("    VbiosRomHeader:     %zu bytes\n", sizeof(VbiosRomHeader));
    printf("    VbiosPcirHeader:    %zu bytes\n", sizeof(VbiosPcirHeader));
}

// ========================================================================
// Main
// ========================================================================
int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <vbios.rom>\n", argv[0]);
        return 1;
    }

    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror("fopen"); return 1; }
    fseek(f, 0, SEEK_END);
    romSize = ftell(f);
    fseek(f, 0, SEEK_SET);
    romData = (uint8_t*)malloc(romSize);
    fread(romData, 1, romSize, f);
    fclose(f);

    printf("NVDAAL VBIOS Parser Test\n");
    printf("ROM: %s (%zu bytes)\n", argv[1], romSize);

    testRomImages();
    testBitHeader();
    testFalconData();
    testAdaFwsecDiscovery();
    testStructLayouts();

    printf("\n========================================\n");
    printf("Results: %d passed, %d failed\n", testsPassed, testsFailed);
    printf("========================================\n");

    free(romData);
    return testsFailed > 0 ? 1 : 0;
}
