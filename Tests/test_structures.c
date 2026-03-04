/**
 * @file test_structures.c
 * @brief Unit tests for NVDAAL data structures and constants
 *
 * Tests structure sizes, alignments, and field offsets to ensure
 * binary compatibility with hardware and firmware.
 *
 * Compile: make test-structures
 * Run: ./Build/test_structures
 */

#include "nvdaal_test.h"
#include <stddef.h>

// Include NVDAAL structures (shared header for user-space testing)
// We define types to match kernel structures without IOKit dependencies
typedef uint8_t  UInt8;
typedef uint16_t UInt16;
typedef uint32_t UInt32;
typedef uint64_t UInt64;

// Pull in the register definitions
#include "../Sources/NVDAALRegs.h"

// ============================================================================
// Structure Size Tests
// ============================================================================

void test_vbios_rom_header_size(void) {
    TEST_ASSERT_EQ(0x1A, sizeof(struct VbiosRomHeader));
}

void test_vbios_pcir_header_size(void) {
    TEST_ASSERT_EQ(0x18, sizeof(struct VbiosPcirHeader));
}

void test_bit_header_size(void) {
    // BIT header: id(2) + sig(4) + ver(2) + hsize(1) + tsize(1) + tcount(1) + flags(1) = 12
    TEST_ASSERT_EQ(12, sizeof(struct BitHeader));
}

void test_bit_token_size(void) {
    // BIT token: id(1) + ver(1) + size(2) + offset(2) = 6
    TEST_ASSERT_EQ(6, sizeof(struct BitToken));
}

void test_pmu_lookup_header_size(void) {
    TEST_ASSERT_EQ(6, sizeof(struct PmuLookupTableHeader));
}

void test_pmu_lookup_entry_ada_size(void) {
    // Ada entry: appId(2) + offset(4) = 6
    TEST_ASSERT_EQ(6, sizeof(struct PmuLookupEntryAda));
}

void test_falcon_ucode_desc_v3_nvidia_size(void) {
    // Must be exactly 44 bytes per NVIDIA spec
    TEST_ASSERT_EQ(FALCON_UCODE_DESC_V3_SIZE, sizeof(struct FalconUcodeDescV3Nvidia));
    TEST_ASSERT_EQ(44, sizeof(struct FalconUcodeDescV3Nvidia));
}

void test_nvfw_bin_hdr_size(void) {
    // vendorId(2) + version(2) + reserved(4) + totalSize(4) +
    // headerOffset(4) + headerSize(4) + dataOffset(4) + dataSize(4) = 28
    TEST_ASSERT_EQ(28, sizeof(struct NvfwBinHdr));
}

void test_dmem_mapper_header_size(void) {
    // sig(4) + ver(2) + size(2) + cmdOff(4) + cmdSize(4) +
    // dataOff(4) + dataSize(4) + initCmd(4) + reserved(32) + interfaceOffset(4) = 64
    TEST_ASSERT_EQ(64, sizeof(struct DmemMapperHeader));
}

void test_fwsec_frts_cmd_size(void) {
    // ReadVbiosDesc(24) + FrtsRegionDesc(20) = 44
    TEST_ASSERT_EQ(24, sizeof(struct FwsecReadVbiosDesc));
    TEST_ASSERT_EQ(20, sizeof(struct FwsecFrtsRegionDesc));
    TEST_ASSERT_EQ(44, sizeof(struct FwsecFrtsCmd));
}

void test_rpc_header_size(void) {
    // sig(4) + hdrVer(4) + result(4) + resultPriv(4) + func(4) + len(4) = 24
    TEST_ASSERT_EQ(24, sizeof(NvRpcMessageHeader));
}

void test_wpr_meta_size(void) {
    // Verify WPR metadata structure size for GSP boot
    // This is critical for correct memory layout
    TEST_ASSERT_IN_RANGE(sizeof(GspFwWprMeta), 120, 160);
}

// ============================================================================
// Field Offset Tests
// ============================================================================

void test_pcir_field_offsets(void) {
    TEST_ASSERT_EQ(0, offsetof(struct VbiosPcirHeader, signature));
    TEST_ASSERT_EQ(4, offsetof(struct VbiosPcirHeader, vendorId));
    TEST_ASSERT_EQ(6, offsetof(struct VbiosPcirHeader, deviceId));
    TEST_ASSERT_EQ(0x10, offsetof(struct VbiosPcirHeader, imageLength));
    TEST_ASSERT_EQ(0x14, offsetof(struct VbiosPcirHeader, codeType));
    TEST_ASSERT_EQ(0x15, offsetof(struct VbiosPcirHeader, indicator));
}

void test_falcon_desc_v3_nvidia_offsets(void) {
    // FalconUcodeDescV3Nvidia layout (44 bytes total):
    // vDesc(4) + storedSize(4) + pkcDataOffset(4) + interfaceOffset(4) +
    // imemPhysBase(4) + imemLoadSize(4) + imemVirtBase(4) +
    // dmemPhysBase(4) + dmemLoadSize(4) + engineIdMask(2) +
    // ucodeId(1) + signatureCount(1) + signatureVersions(2) + reserved(2)
    TEST_ASSERT_EQ(0, offsetof(struct FalconUcodeDescV3Nvidia, vDesc));
    TEST_ASSERT_EQ(4, offsetof(struct FalconUcodeDescV3Nvidia, storedSize));
    TEST_ASSERT_EQ(8, offsetof(struct FalconUcodeDescV3Nvidia, pkcDataOffset));
    TEST_ASSERT_EQ(12, offsetof(struct FalconUcodeDescV3Nvidia, interfaceOffset));
    TEST_ASSERT_EQ(36, offsetof(struct FalconUcodeDescV3Nvidia, engineIdMask));
    TEST_ASSERT_EQ(38, offsetof(struct FalconUcodeDescV3Nvidia, ucodeId));
    TEST_ASSERT_EQ(39, offsetof(struct FalconUcodeDescV3Nvidia, signatureCount));
    TEST_ASSERT_EQ(40, offsetof(struct FalconUcodeDescV3Nvidia, signatureVersions));
}

void test_dmem_mapper_offsets(void) {
    TEST_ASSERT_EQ(0, offsetof(struct DmemMapperHeader, signature));
    TEST_ASSERT_EQ(4, offsetof(struct DmemMapperHeader, version));
    TEST_ASSERT_EQ(24, offsetof(struct DmemMapperHeader, initCmd));
}

// ============================================================================
// Constant Validation Tests
// ============================================================================

void test_vbios_constants(void) {
    TEST_ASSERT_EQ(0xAA55, VBIOS_ROM_SIGNATURE);
    TEST_ASSERT_EQ(0x52494350, PCIR_SIGNATURE);  // "PCIR"
    TEST_ASSERT_EQ(0x00544942, BIT_HEADER_SIGNATURE);  // "BIT\0"
    TEST_ASSERT_EQ(0xB8FF, BIT_HEADER_ID);
}

void test_fwsec_constants(void) {
    TEST_ASSERT_EQ(0x85, FWSEC_APP_ID_FWSEC);
    TEST_ASSERT_EQ(0x15, DMEMMAPPER_CMD_FRTS);
    TEST_ASSERT_EQ(0x50414D44, DMEMMAPPER_SIGNATURE);  // "DMAP"
    TEST_ASSERT_EQ(384, BCRT30_RSA3K_SIG_SIZE);
}

void test_bit_token_ids(void) {
    TEST_ASSERT_EQ(0x50, BIT_TOKEN_PMU_TABLE);  // Ada Lovelace
    TEST_ASSERT_EQ(0x70, BIT_TOKEN_FALCON_DATA);  // Pre-Ada
    TEST_ASSERT_EQ(0x43, BIT_TOKEN_CLOCK_PTRS);  // 'C'
}

void test_register_bases(void) {
    TEST_ASSERT_EQ(0x00110000, NV_PGSP_BASE);
    TEST_ASSERT_EQ(0x00840000, NV_PSEC_BASE);
    TEST_ASSERT_EQ(0x00118000, NV_PRISCV_RISCV_BASE);
    TEST_ASSERT_EQ(0x00180000, NV_PROM_BASE);
}

void test_falcon_register_offsets(void) {
    TEST_ASSERT_EQ(0x0040, FALCON_MAILBOX0);
    TEST_ASSERT_EQ(0x0044, FALCON_MAILBOX1);
    TEST_ASSERT_EQ(0x0100, FALCON_CPUCTL);
    TEST_ASSERT_EQ(0x0104, FALCON_BOOTVEC);
    TEST_ASSERT_EQ(0x0048, FALCON_ITFEN);
}

void test_rpc_signature(void) {
    TEST_ASSERT_EQ(0x43505256, NV_VGPU_MSG_SIGNATURE_VALID);  // "VRPC"
}

void test_chip_architecture_values(void) {
    TEST_ASSERT_EQ(0x17, NV_CHIP_ARCH_AMPERE);
    TEST_ASSERT_EQ(0x19, NV_CHIP_ARCH_ADA);
    TEST_ASSERT_EQ(0x1B, NV_CHIP_ARCH_BLACKWELL);
}

// ============================================================================
// Macro Tests
// ============================================================================

void test_falcon_register_macros(void) {
    // Test IMEMC/DMEMC index macros
    TEST_ASSERT_EQ(0x0180, FALCON_IMEMC(0));
    TEST_ASSERT_EQ(0x0190, FALCON_IMEMC(1));
    TEST_ASSERT_EQ(0x01C0, FALCON_DMEMC(0));
    TEST_ASSERT_EQ(0x01C8, FALCON_DMEMC(1));

    // Test IMEMD/DMEMD index macros
    TEST_ASSERT_EQ(0x0184, FALCON_IMEMD(0));
    TEST_ASSERT_EQ(0x01C4, FALCON_DMEMD(0));
}

void test_wpr2_enabled_macro(void) {
    TEST_ASSERT_EQ(1, NV_PFB_WPR2_ENABLED(0x80000000));
    TEST_ASSERT_EQ(0, NV_PFB_WPR2_ENABLED(0x7FFFFFFF));
    TEST_ASSERT_EQ(1, NV_PFB_WPR2_ENABLED(0xFFFFFFFF));
}

void test_prom_data_macro(void) {
    TEST_ASSERT_EQ(0x00180000, NV_PROM_DATA(0));
    TEST_ASSERT_EQ(0x00180100, NV_PROM_DATA(0x100));
    TEST_ASSERT_EQ(0x001FFFFF, NV_PROM_DATA(0x7FFFF));
}

// ============================================================================
// Binary Pattern Tests (for parsing validation)
// ============================================================================

void test_bit_header_detection(void) {
    // Create a mock BIT header
    uint8_t mockBit[] = {
        0xFF, 0xB8,                     // id = 0xB8FF
        0x42, 0x49, 0x54, 0x00,         // "BIT\0"
        0x01, 0x00,                     // version = 1
        0x0C,                           // headerSize = 12
        0x06,                           // tokenSize = 6
        0x10,                           // tokenCount = 16
        0x00                            // flags = 0
    };

    struct BitHeader *hdr = (struct BitHeader *)mockBit;
    TEST_ASSERT_EQ(BIT_HEADER_ID, hdr->id);
    TEST_ASSERT_EQ(BIT_HEADER_SIGNATURE, hdr->signature);
    TEST_ASSERT_EQ(12, hdr->headerSize);
    TEST_ASSERT_EQ(6, hdr->tokenSize);
    TEST_ASSERT_EQ(16, hdr->tokenCount);
}

void test_pcir_header_detection(void) {
    // Create a mock PCIR header
    uint8_t mockPcir[] = {
        0x50, 0x43, 0x49, 0x52,         // "PCIR"
        0xDE, 0x10,                     // vendorId = 0x10DE (NVIDIA)
        0x84, 0x26,                     // deviceId = 0x2684 (RTX 4090)
        0x00, 0x00,                     // vpdOffset
        0x18, 0x00,                     // length = 24
        0x03,                           // revision
        0x00, 0x03, 0x00,               // classCode (VGA)
        0x00, 0x02,                     // imageLength = 512 blocks = 256KB
        0x00, 0x00,                     // codeRevision
        0x00,                           // codeType = PCI/AT
        0x00,                           // indicator
        0x00, 0x00                      // maxRuntimeSize
    };

    struct VbiosPcirHeader *pcir = (struct VbiosPcirHeader *)mockPcir;
    TEST_ASSERT_EQ(PCIR_SIGNATURE, pcir->signature);
    TEST_ASSERT_EQ(0x10DE, pcir->vendorId);
    TEST_ASSERT_EQ(0x2684, pcir->deviceId);
    TEST_ASSERT_EQ(0x00, pcir->codeType);
}

void test_pmu_ada_entry_parsing(void) {
    // Ada PMU entry: 16-bit appId + 32-bit offset
    uint8_t mockEntry[] = {
        0x85, 0x00,                     // appId = 0x0085 (FWSEC)
        0x00, 0xA0, 0x01, 0x00          // offset = 0x0001A000
    };

    struct PmuLookupEntryAda *entry = (struct PmuLookupEntryAda *)mockEntry;
    TEST_ASSERT_EQ(0x0085, entry->appId);
    TEST_ASSERT_EQ(0x0001A000, entry->dataOffset);
}

void test_dmemmapper_detection(void) {
    // Mock DMEMMAPPER header
    uint8_t mockDmap[] = {
        0x44, 0x4D, 0x41, 0x50,         // "DMAP"
        0x03, 0x00,                     // version = 3
        0x3C, 0x00,                     // size = 60
        // ... rest not needed for signature test
    };

    struct DmemMapperHeader *dmap = (struct DmemMapperHeader *)mockDmap;
    TEST_ASSERT_EQ(DMEMMAPPER_SIGNATURE, dmap->signature);
    TEST_ASSERT_EQ(3, dmap->version);
}

// ============================================================================
// Alignment Tests
// ============================================================================

void test_structure_alignment(void) {
    // Verify structures are properly packed (no padding)
    TEST_ASSERT_EQ(0, sizeof(struct BitToken) % 2);  // 2-byte aligned
    TEST_ASSERT_EQ(0, sizeof(struct PmuLookupEntryAda) % 2);
    TEST_ASSERT_EQ(0, sizeof(struct FalconUcodeDescV3Nvidia) % 4);  // 4-byte aligned
}

// ============================================================================
// RM Class Tests
// ============================================================================

void test_rm_class_values(void) {
    TEST_ASSERT_EQ(0x00000000, NV01_ROOT);
    TEST_ASSERT_EQ(0x00000041, NV01_ROOT_CLIENT);
    TEST_ASSERT_EQ(0x000090F1, FERMI_VASPACE_A);
    TEST_ASSERT_EQ(0x0000C96F, ADA_CHANNEL_GPFIFO_A);
    TEST_ASSERT_EQ(0x0000C9C0, AD102_COMPUTE_A);
}

void test_engine_types(void) {
    TEST_ASSERT_EQ(0, NV2080_ENGINE_TYPE_GRAPHICS);
    TEST_ASSERT_EQ(1, NV2080_ENGINE_TYPE_COMPUTE);
    TEST_ASSERT_EQ(2, NV2080_ENGINE_TYPE_COPY);
}

// ============================================================================
// Main
// ============================================================================

TEST_MAIN("NVDAAL Structure Tests",
    // Structure sizes
    TEST_CASE(test_vbios_rom_header_size),
    TEST_CASE(test_vbios_pcir_header_size),
    TEST_CASE(test_bit_header_size),
    TEST_CASE(test_bit_token_size),
    TEST_CASE(test_pmu_lookup_header_size),
    TEST_CASE(test_pmu_lookup_entry_ada_size),
    TEST_CASE(test_falcon_ucode_desc_v3_nvidia_size),
    TEST_CASE(test_nvfw_bin_hdr_size),
    TEST_CASE(test_dmem_mapper_header_size),
    TEST_CASE(test_fwsec_frts_cmd_size),
    TEST_CASE(test_rpc_header_size),
    TEST_CASE(test_wpr_meta_size),

    // Field offsets
    TEST_CASE(test_pcir_field_offsets),
    TEST_CASE(test_falcon_desc_v3_nvidia_offsets),
    TEST_CASE(test_dmem_mapper_offsets),

    // Constants
    TEST_CASE(test_vbios_constants),
    TEST_CASE(test_fwsec_constants),
    TEST_CASE(test_bit_token_ids),
    TEST_CASE(test_register_bases),
    TEST_CASE(test_falcon_register_offsets),
    TEST_CASE(test_rpc_signature),
    TEST_CASE(test_chip_architecture_values),

    // Macros
    TEST_CASE(test_falcon_register_macros),
    TEST_CASE(test_wpr2_enabled_macro),
    TEST_CASE(test_prom_data_macro),

    // Binary patterns
    TEST_CASE(test_bit_header_detection),
    TEST_CASE(test_pcir_header_detection),
    TEST_CASE(test_pmu_ada_entry_parsing),
    TEST_CASE(test_dmemmapper_detection),

    // Alignment
    TEST_CASE(test_structure_alignment),

    // RM Classes
    TEST_CASE(test_rm_class_values),
    TEST_CASE(test_engine_types)
)
