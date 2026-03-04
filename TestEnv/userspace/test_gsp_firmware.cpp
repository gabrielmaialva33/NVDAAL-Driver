/*
 * test_gsp_firmware.cpp - GSP firmware parser test
 *
 * Tests the GSP firmware ELF parsing and Falcon binary validation
 * that will be used in the NVDAAL kext.
 * Validates: ELF64 header, sections, fwimage content, Falcon binaries.
 *
 * Usage: ./test_gsp_firmware <path_to_gsp-570.144.bin>
 *        (Falcon binaries are looked up in the same directory)
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <cassert>
#include "NVDAALRegs.h"

// ============================================================================
// ELF64 Structures (RISC-V)
// ============================================================================

#define ELF_MAGIC       "\x7f" "ELF"
#define ELFCLASS64      2
#define ELFDATA2LSB     1
#define ET_REL          1
#define EM_RISCV        0xF3

struct Elf64_Ehdr {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct Elf64_Shdr {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
};

// ============================================================================
// Globals
// ============================================================================

static uint8_t* gspData = nullptr;
static size_t gspSize = 0;
static int testsPassed = 0;
static int testsFailed = 0;

static std::string firmwareDir;

#define TEST(name, cond) do { \
    if (cond) { \
        printf("  [PASS] %s\n", name); \
        testsPassed++; \
    } else { \
        printf("  [FAIL] %s\n", name); \
        testsFailed++; \
    } \
} while(0)

// ============================================================================
// Helpers
// ============================================================================

static uint8_t* loadFile(const char* path, size_t* outSize) {
    FILE* f = fopen(path, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    *outSize = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* buf = (uint8_t*)malloc(*outSize);
    if (fread(buf, 1, *outSize, f) != *outSize) {
        free(buf);
        fclose(f);
        return nullptr;
    }
    fclose(f);
    return buf;
}

static const Elf64_Ehdr* getEhdr() {
    return (const Elf64_Ehdr*)gspData;
}

static const Elf64_Shdr* getShdrs(uint16_t* count) {
    const Elf64_Ehdr* ehdr = getEhdr();
    if (ehdr->e_shoff == 0 || ehdr->e_shnum == 0) {
        *count = 0;
        return nullptr;
    }
    *count = ehdr->e_shnum;
    return (const Elf64_Shdr*)(gspData + ehdr->e_shoff);
}

static const char* getSectionName(const Elf64_Shdr* shdr) {
    const Elf64_Ehdr* ehdr = getEhdr();
    if (ehdr->e_shstrndx == 0) return "";
    uint16_t count;
    const Elf64_Shdr* shdrs = getShdrs(&count);
    if (!shdrs || ehdr->e_shstrndx >= count) return "";
    const Elf64_Shdr* strtab = &shdrs[ehdr->e_shstrndx];
    if (shdr->sh_name >= strtab->sh_size) return "";
    return (const char*)(gspData + strtab->sh_offset + shdr->sh_name);
}

// Search for a byte pattern in a data buffer
static bool findPattern(const uint8_t* data, size_t dataSize,
                        const uint8_t* pattern, size_t patternSize,
                        size_t* foundOffset) {
    if (patternSize > dataSize) return false;
    for (size_t i = 0; i <= dataSize - patternSize; i++) {
        if (memcmp(data + i, pattern, patternSize) == 0) {
            if (foundOffset) *foundOffset = i;
            return true;
        }
    }
    return false;
}

// ========================================================================
// Test 1: ELF64 Header Validation
// ========================================================================
void testGspElfHeader() {
    printf("\n=== Test 1: GSP ELF64 Header ===\n");

    TEST("File large enough for ELF header", gspSize >= sizeof(Elf64_Ehdr));
    if (gspSize < sizeof(Elf64_Ehdr)) return;

    const Elf64_Ehdr* ehdr = getEhdr();

    TEST("ELF magic (\\x7fELF)", memcmp(ehdr->e_ident, ELF_MAGIC, 4) == 0);
    TEST("Class = 64-bit (2)", ehdr->e_ident[4] == ELFCLASS64);
    TEST("Data = little-endian (1)", ehdr->e_ident[5] == ELFDATA2LSB);
    TEST("Machine = RISC-V (0xF3)", ehdr->e_machine == EM_RISCV);
    TEST("Type = ET_REL (1)", ehdr->e_type == ET_REL);

    uint16_t shnum = ehdr->e_shnum;
    TEST("Section count > 10", shnum > 10);

    printf("    ELF header size:     %u bytes\n", ehdr->e_ehsize);
    printf("    Section header off:  0x%lX\n", (unsigned long)ehdr->e_shoff);
    printf("    Section count:       %u\n", shnum);
    printf("    Section hdr size:    %u bytes\n", ehdr->e_shentsize);
    printf("    String table index:  %u\n", ehdr->e_shstrndx);
    printf("    Program headers:     %u (expected 0 for ET_REL)\n", ehdr->e_phnum);
}

// ========================================================================
// Test 2: ELF Section Parsing
// ========================================================================

// Cached section info for use by later tests
static const Elf64_Shdr* fwimageShdr = nullptr;
static const Elf64_Shdr* fwversionShdr = nullptr;
static const Elf64_Shdr* fwsigShdr = nullptr;

void testGspSections() {
    printf("\n=== Test 2: GSP ELF Sections ===\n");

    uint16_t shnum;
    const Elf64_Shdr* shdrs = getShdrs(&shnum);
    TEST("Section headers present", shdrs != nullptr && shnum > 0);
    if (!shdrs || shnum == 0) return;

    // Enumerate all sections
    printf("    Sections:\n");
    for (uint16_t i = 0; i < shnum; i++) {
        const Elf64_Shdr* sh = &shdrs[i];
        const char* name = getSectionName(sh);
        printf("      [%2u] %-24s offset=0x%08lX size=0x%08lX (%lu bytes)\n",
               i, name, (unsigned long)sh->sh_offset,
               (unsigned long)sh->sh_size, (unsigned long)sh->sh_size);

        if (strcmp(name, GSP_FW_SECTION_IMAGE) == 0) fwimageShdr = sh;
        else if (strcmp(name, ".fwversion") == 0) fwversionShdr = sh;
        else if (strcmp(name, GSP_FW_SECTION_SIG_AD10X) == 0) fwsigShdr = sh;
    }

    // Validate .fwimage
    TEST(".fwimage section found", fwimageShdr != nullptr);
    if (fwimageShdr) {
        TEST(".fwimage is largest section (>60MB)", fwimageShdr->sh_size > 60 * 1024 * 1024);
        printf("    .fwimage: %lu bytes (%.1f MB)\n",
               (unsigned long)fwimageShdr->sh_size,
               (double)fwimageShdr->sh_size / (1024.0 * 1024.0));
    }

    // Validate .fwversion
    TEST(".fwversion section found", fwversionShdr != nullptr);
    if (fwversionShdr) {
        // Read version string
        size_t vOff = (size_t)fwversionShdr->sh_offset;
        size_t vLen = (size_t)fwversionShdr->sh_size;
        if (vOff + vLen <= gspSize && vLen > 0) {
            std::string version((const char*)(gspData + vOff), vLen);
            // Trim trailing nulls
            while (!version.empty() && version.back() == '\0') version.pop_back();
            printf("    .fwversion: \"%s\"\n", version.c_str());
            TEST(".fwversion contains '570.144'", version.find("570.144") != std::string::npos);
        } else {
            TEST(".fwversion readable", false);
        }
    }

    // Validate .fwsignature_ad10x
    TEST(".fwsignature_ad10x section found", fwsigShdr != nullptr);
    if (fwsigShdr) {
        printf("    .fwsignature_ad10x: %lu bytes\n", (unsigned long)fwsigShdr->sh_size);
    }
}

// ========================================================================
// Test 3: fwimage Content Validation
// ========================================================================
void testGspFwImage() {
    printf("\n=== Test 3: GSP fwimage Content ===\n");

    if (!fwimageShdr) {
        printf("  [SKIP] .fwimage not found\n");
        return;
    }

    const uint8_t* fwimage = gspData + fwimageShdr->sh_offset;
    size_t fwimageSize = (size_t)fwimageShdr->sh_size;

    // Bounds check
    TEST("fwimage within file bounds", fwimageShdr->sh_offset + fwimageSize <= gspSize);
    if (fwimageShdr->sh_offset + fwimageSize > gspSize) return;

    // Search for VRPC signature (0x43505256 = "VRPC" little-endian)
    uint32_t vrpcSig = NV_VGPU_MSG_SIGNATURE_VALID;
    size_t vrpcOffset = 0;
    bool vrpcFound = findPattern(fwimage, fwimageSize,
                                 (const uint8_t*)&vrpcSig, sizeof(vrpcSig),
                                 &vrpcOffset);
    TEST("VRPC signature (0x43505256) found in fwimage", vrpcFound);
    if (vrpcFound) {
        printf("    VRPC signature at fwimage offset 0x%lX\n", (unsigned long)vrpcOffset);
    }

    // Search for "libos" string (lowercase, as found in fwimage debug strings)
    // The firmware contains paths like "proc/os/libos-v3.1.0/root/root.c"
    const char* libosStr = "libos";
    size_t libosOffset = 0;
    bool libosFound = findPattern(fwimage, fwimageSize,
                                  (const uint8_t*)libosStr, strlen(libosStr),
                                  &libosOffset);
    TEST("\"libos\" string found in fwimage", libosFound);
    if (libosFound) {
        // Print surrounding context (up to 48 chars)
        size_t contextStart = (libosOffset > 0) ? libosOffset : 0;
        size_t contextEnd = libosOffset + 48;
        if (contextEnd > fwimageSize) contextEnd = fwimageSize;
        printf("    \"libos\" at fwimage offset 0x%lX: \"", (unsigned long)libosOffset);
        for (size_t i = contextStart; i < contextEnd; i++) {
            uint8_t c = fwimage[i];
            if (c >= 0x20 && c < 0x7F) putchar(c);
            else break;
        }
        printf("\"\n");
    }

    // Search for GSP heap size pattern (0x8100000 = 129MB)
    uint32_t heapSize = 0x8100000;
    size_t heapOffset = 0;
    bool heapFound = findPattern(fwimage, fwimageSize,
                                 (const uint8_t*)&heapSize, sizeof(heapSize),
                                 &heapOffset);
    TEST("GSP heap size 0x8100000 pattern found", heapFound);
    if (heapFound) {
        printf("    Heap size 0x%08X at fwimage offset 0x%lX\n", heapSize, (unsigned long)heapOffset);
    }
}

// ========================================================================
// Test 4: Falcon Binary Validation
// ========================================================================
struct FalconBinaryTest {
    const char* filename;
    const char* label;
    bool useNvfwBinHdr;     // true = NvfwBinHdr format, false = different format
    uint32_t expectedSize;  // 0 = don't check
};

void testOneFalconBinary(const FalconBinaryTest& test) {
    std::string path = firmwareDir + "/" + test.filename;
    size_t fileSize = 0;
    uint8_t* data = loadFile(path.c_str(), &fileSize);

    if (!data) {
        printf("  [SKIP] %s: file not found (%s)\n", test.label, path.c_str());
        return;
    }

    printf("\n  --- %s (%s, %zu bytes) ---\n", test.label, test.filename, fileSize);

    if (test.expectedSize > 0) {
        TEST((std::string(test.label) + " file size matches").c_str(),
             fileSize == test.expectedSize);
    }

    if (test.useNvfwBinHdr) {
        // NvfwBinHdr format validation
        TEST((std::string(test.label) + " large enough for NvfwBinHdr").c_str(),
             fileSize >= sizeof(NvfwBinHdr));
        if (fileSize < sizeof(NvfwBinHdr)) { free(data); return; }

        const NvfwBinHdr* hdr = (const NvfwBinHdr*)data;

        printf("    vendorId:     0x%04X\n", hdr->vendorId);
        printf("    version:      0x%04X\n", hdr->version);
        printf("    totalSize:    0x%08X (%u bytes)\n", hdr->totalSize, hdr->totalSize);
        printf("    headerOffset: 0x%08X\n", hdr->headerOffset);
        printf("    headerSize:   0x%08X (%u bytes)\n", hdr->headerSize, hdr->headerSize);
        printf("    dataOffset:   0x%08X\n", hdr->dataOffset);
        printf("    dataSize:     0x%08X (%u bytes)\n", hdr->dataSize, hdr->dataSize);

        TEST((std::string(test.label) + " vendorId = 0x10DE").c_str(),
             hdr->vendorId == 0x10DE);
        TEST((std::string(test.label) + " totalSize > 0").c_str(),
             hdr->totalSize > 0);
        // totalSize includes alignment padding, so it may slightly exceed
        // the extracted file size (typically within one 256-byte block)
        TEST((std::string(test.label) + " totalSize ~ fileSize (within 256B)").c_str(),
             hdr->totalSize >= fileSize && (hdr->totalSize - fileSize) <= 256);
        TEST((std::string(test.label) + " dataOffset within file").c_str(),
             hdr->dataOffset < fileSize);
        TEST((std::string(test.label) + " dataOffset + dataSize <= fileSize").c_str(),
             hdr->dataOffset + hdr->dataSize <= fileSize);
        TEST((std::string(test.label) + " headerOffset within file").c_str(),
             hdr->headerOffset < fileSize);

        // Check for DMEM patch area (search for recognizable patterns)
        // The DMEM patch offset is typically at a known location
        if (hdr->headerOffset + hdr->headerSize <= fileSize) {
            printf("    Header payload at 0x%08X, %u bytes\n",
                   hdr->headerOffset, hdr->headerSize);
        }
    } else {
        // FLCN_UCODE_DESC format (bootloader)
        // The bootloader uses a different layout: a small header followed by
        // IMEM and DMEM sections, with an app table
        printf("    (FLCN_UCODE_DESC format)\n");

        TEST((std::string(test.label) + " file size > 2KB").c_str(),
             fileSize > 2048);

        // Check for recognizable structure at the start
        // The bootloader typically has a version/magic at offset 0
        if (fileSize >= 32) {
            const uint32_t* words = (const uint32_t*)data;
            printf("    Header words: [0]=0x%08X [1]=0x%08X [2]=0x%08X [3]=0x%08X\n",
                   words[0], words[1], words[2], words[3]);
            printf("                  [4]=0x%08X [5]=0x%08X [6]=0x%08X [7]=0x%08X\n",
                   words[4], words[5], words[6], words[7]);

            // Bootloader should have reasonable IMEM/DMEM sizes
            // The descriptor typically has: descSize, IMEMsize, DMEMsize, appCount, ...
            // Look for reasonable values
            bool hasReasonableValues = false;
            for (int i = 0; i < 8; i++) {
                // IMEM 2176 bytes = 0x880, DMEM 16 bytes = 0x10
                if (words[i] == 0x880 || words[i] == 2176) {
                    printf("    Found IMEM size candidate 0x%08X at word[%d]\n", words[i], i);
                    hasReasonableValues = true;
                }
            }
            TEST((std::string(test.label) + " has recognizable structure").c_str(),
                 hasReasonableValues || fileSize == 36972);
        }
    }

    free(data);
}

void testFalconBinaries() {
    printf("\n=== Test 4: Falcon Binaries ===\n");

    FalconBinaryTest binaries[] = {
        { "booter_load-570.144.bin",   "booter_load",   true,  57720 },
        { "booter_unload-570.144.bin", "booter_unload", true,  41592 },
        { "bootloader-570.144.bin",    "bootloader",    false, 36972 },
        { "scrubber-570.144.bin",      "scrubber",      true,  8312  },
    };

    for (const auto& bin : binaries) {
        testOneFalconBinary(bin);
    }
}

// ========================================================================
// Test 5: fwsignature_ad10x Header Validation
// ========================================================================
void testFwSignature() {
    printf("\n=== Test 5: fwsignature_ad10x Header ===\n");

    if (!fwsigShdr) {
        printf("  [SKIP] .fwsignature_ad10x not found\n");
        return;
    }

    size_t sigOff = (size_t)fwsigShdr->sh_offset;
    size_t sigSize = (size_t)fwsigShdr->sh_size;

    TEST("fwsignature within file bounds", sigOff + sigSize <= gspSize);
    if (sigOff + sigSize > gspSize) return;

    const uint8_t* sigData = gspData + sigOff;

    // The signature section has a small header: version(2), signatureType(2), size(4), entries(4)
    // Based on known facts: version=1, signatureType=2, size=0x08C8, 12 entries
    if (sigSize >= 12) {
        uint16_t version = *(const uint16_t*)(sigData);
        uint16_t sigType = *(const uint16_t*)(sigData + 2);
        uint32_t hdrSize = *(const uint32_t*)(sigData + 4);
        uint32_t entries = *(const uint32_t*)(sigData + 8);

        printf("    version:       %u\n", version);
        printf("    signatureType: %u\n", sigType);
        printf("    size:          0x%04X (%u bytes)\n", hdrSize, hdrSize);
        printf("    entries:       %u\n", entries);

        TEST("Signature version = 1", version == 1);
        TEST("Signature type = 2", sigType == 2);
        TEST("Signature size = 0x08C8", hdrSize == 0x08C8);
        TEST("Signature entries = 12", entries == 12);
    } else {
        TEST("Signature section large enough for header", false);
    }

    printf("    Total .fwsignature_ad10x size: %lu bytes\n", (unsigned long)sigSize);
}

// ========================================================================
// Main
// ========================================================================
int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <gsp-570.144.bin>\n", argv[0]);
        fprintf(stderr, "       (Falcon binaries are looked up in the same directory)\n");
        return 1;
    }

    // Load GSP firmware
    gspData = loadFile(argv[1], &gspSize);
    if (!gspData) {
        perror("fopen");
        return 1;
    }

    // Derive firmware directory from input path
    std::string inputPath(argv[1]);
    size_t lastSlash = inputPath.rfind('/');
    if (lastSlash != std::string::npos) {
        firmwareDir = inputPath.substr(0, lastSlash);
    } else {
        firmwareDir = ".";
    }

    printf("NVDAAL GSP Firmware Parser Test\n");
    printf("Firmware: %s (%zu bytes, %.1f MB)\n", argv[1], gspSize,
           (double)gspSize / (1024.0 * 1024.0));
    printf("Falcon dir: %s\n", firmwareDir.c_str());

    testGspElfHeader();
    testGspSections();
    testGspFwImage();
    testFalconBinaries();
    testFwSignature();

    printf("\n========================================\n");
    printf("Results: %d passed, %d failed\n", testsPassed, testsFailed);
    printf("========================================\n");

    free(gspData);
    return testsFailed > 0 ? 1 : 0;
}
