/*
 * test_rpc_structs.cpp - RPC struct layout and constant validation
 *
 * Validates compile-time properties of the structs and definitions in
 * NVDAALRegs.h: struct sizes, field offsets, RPC protocol constants,
 * register offsets, and Ada Lovelace specific values.
 *
 * Usage: ./test_rpc_structs
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstddef>
#include <cstdint>
#include "NVDAALRegs.h"

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

// ========================================================================
// Test 1: Struct Sizes
// ========================================================================
void testStructSizes() {
    printf("\n=== Test 1: Struct Sizes ===\n");

    TEST("NvRpcMessageHeader == 24",
         sizeof(NvRpcMessageHeader) == 24);
    printf("    NvRpcMessageHeader: %zu bytes\n", sizeof(NvRpcMessageHeader));

    TEST("GspQueueElement == 16 (header only, flexible array)",
         sizeof(GspQueueElement) == 16);
    printf("    GspQueueElement: %zu bytes\n", sizeof(GspQueueElement));

    TEST("GspFwWprMeta == 144",
         sizeof(GspFwWprMeta) == 144);
    printf("    GspFwWprMeta: %zu bytes\n", sizeof(GspFwWprMeta));

    TEST("GspLibosInitArgs == 40",
         sizeof(GspLibosInitArgs) == 40);
    printf("    GspLibosInitArgs: %zu bytes\n", sizeof(GspLibosInitArgs));

    TEST("FalconUcodeDescV3Nvidia == 44",
         sizeof(FalconUcodeDescV3Nvidia) == 44);
    printf("    FalconUcodeDescV3Nvidia: %zu bytes\n", sizeof(FalconUcodeDescV3Nvidia));

    TEST("BitHeader == 12",
         sizeof(BitHeader) == 12);
    printf("    BitHeader: %zu bytes\n", sizeof(BitHeader));

    TEST("BitToken == 6",
         sizeof(BitToken) == 6);
    printf("    BitToken: %zu bytes\n", sizeof(BitToken));

    TEST("VbiosRomHeader == 26",
         sizeof(VbiosRomHeader) == 26);
    printf("    VbiosRomHeader: %zu bytes\n", sizeof(VbiosRomHeader));

    TEST("VbiosPcirHeader == 24",
         sizeof(VbiosPcirHeader) == 24);
    printf("    VbiosPcirHeader: %zu bytes\n", sizeof(VbiosPcirHeader));

    TEST("NvfwBinHdr == 28",
         sizeof(NvfwBinHdr) == 28);
    printf("    NvfwBinHdr: %zu bytes\n", sizeof(NvfwBinHdr));

    // DmemMapperHeader: 64 bytes total, matching hardware DMAP layout
    // signature(4) + version(2) + size(2) + cmdBufOffset(4) + cmdBufSize(4)
    // + dataBufOffset(4) + dataBufSize(4) + initCmd(4) + reserved[8](32) + interfaceOffset(4)
    TEST("DmemMapperHeader == 64",
         sizeof(DmemMapperHeader) == 64);
    printf("    DmemMapperHeader: %zu bytes\n", sizeof(DmemMapperHeader));

    TEST("AdaFalconDescPreHeader == 8",
         sizeof(AdaFalconDescPreHeader) == 8);
    printf("    AdaFalconDescPreHeader: %zu bytes\n", sizeof(AdaFalconDescPreHeader));

    TEST("AdaFalconDescPackedInfo == 4",
         sizeof(AdaFalconDescPackedInfo) == 4);
    printf("    AdaFalconDescPackedInfo: %zu bytes\n", sizeof(AdaFalconDescPackedInfo));

    TEST("AdaFalconDescSizeFields == 16",
         sizeof(AdaFalconDescSizeFields) == 16);
    printf("    AdaFalconDescSizeFields: %zu bytes\n", sizeof(AdaFalconDescSizeFields));

    TEST("FwsecReadVbiosDesc == 24",
         sizeof(FwsecReadVbiosDesc) == 24);
    printf("    FwsecReadVbiosDesc: %zu bytes\n", sizeof(FwsecReadVbiosDesc));

    TEST("FwsecFrtsRegionDesc == 20",
         sizeof(FwsecFrtsRegionDesc) == 20);
    printf("    FwsecFrtsRegionDesc: %zu bytes\n", sizeof(FwsecFrtsRegionDesc));

    // GspBusInfo is NOT inside #pragma pack(push, 1), so the compiler adds
    // 1 byte of tail padding after revisionID for 2-byte alignment (max member = uint16_t).
    TEST("GspBusInfo == 10 (9 payload + 1 padding, not packed)",
         sizeof(GspBusInfo) == 10);
    printf("    GspBusInfo: %zu bytes\n", sizeof(GspBusInfo));

    TEST("GspSystemInfo > 900 bytes (reasonable size)",
         sizeof(GspSystemInfo) > 900);
    printf("    GspSystemInfo: %zu bytes\n", sizeof(GspSystemInfo));
}

// ========================================================================
// Test 2: Critical Field Offsets
// ========================================================================
void testFieldOffsets() {
    printf("\n=== Test 2: Critical Field Offsets ===\n");

    // NvRpcMessageHeader offsets
    TEST("NvRpcMessageHeader::signature at offset 0",
         offsetof(NvRpcMessageHeader, signature) == 0);
    printf("    NvRpcMessageHeader::signature: offset %zu\n",
           offsetof(NvRpcMessageHeader, signature));

    TEST("NvRpcMessageHeader::function at offset 16",
         offsetof(NvRpcMessageHeader, function) == 16);
    printf("    NvRpcMessageHeader::function: offset %zu\n",
           offsetof(NvRpcMessageHeader, function));

    TEST("NvRpcMessageHeader::length at offset 20",
         offsetof(NvRpcMessageHeader, length) == 20);
    printf("    NvRpcMessageHeader::length: offset %zu\n",
           offsetof(NvRpcMessageHeader, length));

    // GspFwWprMeta offsets
    TEST("GspFwWprMeta::magic at offset 0",
         offsetof(GspFwWprMeta, magic) == 0);
    printf("    GspFwWprMeta::magic: offset %zu\n",
           offsetof(GspFwWprMeta, magic));

    TEST("GspFwWprMeta::sysmemAddrOfRadix3Elf at offset 24",
         offsetof(GspFwWprMeta, sysmemAddrOfRadix3Elf) == 24);
    printf("    GspFwWprMeta::sysmemAddrOfRadix3Elf: offset %zu\n",
           offsetof(GspFwWprMeta, sysmemAddrOfRadix3Elf));

    // Offset 72 is gspFwHeapVirtAddr; gspFwHeapSize follows at offset 80.
    TEST("GspFwWprMeta::gspFwHeapSize at offset 80",
         offsetof(GspFwWprMeta, gspFwHeapSize) == 80);
    printf("    GspFwWprMeta::gspFwHeapSize: offset %zu\n",
           offsetof(GspFwWprMeta, gspFwHeapSize));

    // GspSystemInfo offsets
    TEST("GspSystemInfo::gpuPhysAddr at offset 0",
         offsetof(GspSystemInfo, gpuPhysAddr) == 0);
    printf("    GspSystemInfo::gpuPhysAddr: offset %zu\n",
           offsetof(GspSystemInfo, gpuPhysAddr));

    TEST("GspSystemInfo::nvDomainBusDeviceFunc at offset 32",
         offsetof(GspSystemInfo, nvDomainBusDeviceFunc) == 32);
    printf("    GspSystemInfo::nvDomainBusDeviceFunc: offset %zu\n",
           offsetof(GspSystemInfo, nvDomainBusDeviceFunc));

    size_t hypervisorOff = offsetof(GspSystemInfo, hypervisorType);
    TEST("GspSystemInfo::hypervisorType at expected offset",
         hypervisorOff > 0 && hypervisorOff < sizeof(GspSystemInfo));
    printf("    GspSystemInfo::hypervisorType: offset %zu\n", hypervisorOff);

    // DmemMapperHeader offsets
    TEST("DmemMapperHeader::signature at offset 0",
         offsetof(DmemMapperHeader, signature) == 0);
    printf("    DmemMapperHeader::signature: offset %zu\n",
           offsetof(DmemMapperHeader, signature));

    // interfaceOffset at +0x3C (60) matching real hardware DMAP layout
    TEST("DmemMapperHeader::interfaceOffset at offset 60 (0x3C)",
         offsetof(DmemMapperHeader, interfaceOffset) == 60);
    printf("    DmemMapperHeader::interfaceOffset: offset %zu\n",
           offsetof(DmemMapperHeader, interfaceOffset));
}

// ========================================================================
// Test 3: RPC Protocol Constants
// ========================================================================
void testRpcConstants() {
    printf("\n=== Test 3: RPC Protocol Constants ===\n");

    TEST("NV_VGPU_MSG_SIGNATURE_VALID == 0x43505256",
         NV_VGPU_MSG_SIGNATURE_VALID == 0x43505256);

    // Verify it spells "VRPC" in little-endian
    uint32_t sig = NV_VGPU_MSG_SIGNATURE_VALID;
    char sigStr[5];
    memcpy(sigStr, &sig, 4);
    sigStr[4] = '\0';
    TEST("Signature spells 'VRPC' in little-endian",
         sigStr[0] == 'V' && sigStr[1] == 'R' &&
         sigStr[2] == 'P' && sigStr[3] == 'C');
    printf("    Signature string: \"%s\"\n", sigStr);

    TEST("NV_VGPU_MSG_FUNCTION_GSP_SET_SYSTEM_INFO == 0x15",
         NV_VGPU_MSG_FUNCTION_GSP_SET_SYSTEM_INFO == 0x15);

    TEST("NV_VGPU_MSG_FUNCTION_SET_REGISTRY == 0x16",
         NV_VGPU_MSG_FUNCTION_SET_REGISTRY == 0x16);

    TEST("NV_VGPU_MSG_EVENT_GSP_INIT_DONE == 0x52",
         NV_VGPU_MSG_EVENT_GSP_INIT_DONE == 0x52);

    TEST("BIT_HEADER_ID == 0xB8FF",
         BIT_HEADER_ID == 0xB8FF);

    TEST("BIT_HEADER_SIGNATURE == 0x00544942 ('BIT\\0')",
         BIT_HEADER_SIGNATURE == 0x00544942);

    // Verify BIT signature spells "BIT\0"
    uint32_t bitSig = BIT_HEADER_SIGNATURE;
    char bitStr[5];
    memcpy(bitStr, &bitSig, 4);
    bitStr[4] = '\0';
    TEST("BIT signature spells 'BIT' in little-endian",
         bitStr[0] == 'B' && bitStr[1] == 'I' &&
         bitStr[2] == 'T' && bitStr[3] == '\0');
    printf("    BIT signature string: \"%s\"\n", bitStr);

    TEST("DMEMMAPPER_SIGNATURE == 0x50414D44 ('DMAP')",
         DMEMMAPPER_SIGNATURE == 0x50414D44);

    // Verify DMAP signature spells "DMAP"
    uint32_t dmapSig = DMEMMAPPER_SIGNATURE;
    char dmapStr[5];
    memcpy(dmapStr, &dmapSig, 4);
    dmapStr[4] = '\0';
    TEST("DMAP signature spells 'DMAP' in little-endian",
         dmapStr[0] == 'D' && dmapStr[1] == 'M' &&
         dmapStr[2] == 'A' && dmapStr[3] == 'P');
    printf("    DMAP signature string: \"%s\"\n", dmapStr);

    TEST("DMEMMAPPER_CMD_FRTS == 0x15",
         DMEMMAPPER_CMD_FRTS == 0x15);

    TEST("DMEMMAPPER_CMD_SB == 0x19",
         DMEMMAPPER_CMD_SB == 0x19);
}

// ========================================================================
// Test 4: Register Offsets
// ========================================================================
void testRegisterOffsets() {
    printf("\n=== Test 4: Register Offsets ===\n");

    TEST("NV_PMC_BOOT_0 == 0x00000000",
         NV_PMC_BOOT_0 == 0x00000000);

    TEST("NV_PGSP_BASE == 0x00110000",
         NV_PGSP_BASE == 0x00110000);

    TEST("NV_PSEC_BASE == 0x00840000",
         NV_PSEC_BASE == 0x00840000);

    TEST("NV_PRISCV_RISCV_CPUCTL == 0x00118388",
         NV_PRISCV_RISCV_CPUCTL == 0x00118388);

    TEST("NV_PFB_PRI_MMU_WPR2_ADDR_LO == 0x001FA824",
         NV_PFB_PRI_MMU_WPR2_ADDR_LO == 0x001FA824);

    TEST("NV_PFB_PRI_MMU_WPR2_ADDR_HI == 0x001FA828",
         NV_PFB_PRI_MMU_WPR2_ADDR_HI == 0x001FA828);

    TEST("NV_PGSP_QUEUE_HEAD(0) == 0x00110C00",
         NV_PGSP_QUEUE_HEAD(0) == 0x00110C00);
    printf("    NV_PGSP_QUEUE_HEAD(0) = 0x%08X\n", NV_PGSP_QUEUE_HEAD(0));

    TEST("NV_PGSP_QUEUE_TAIL(0) == 0x00110C80",
         NV_PGSP_QUEUE_TAIL(0) == 0x00110C80);
    printf("    NV_PGSP_QUEUE_TAIL(0) = 0x%08X\n", NV_PGSP_QUEUE_TAIL(0));

    TEST("NV_PGSP_QUEUE_HEAD(1) == 0x00110C08",
         NV_PGSP_QUEUE_HEAD(1) == 0x00110C08);
    printf("    NV_PGSP_QUEUE_HEAD(1) = 0x%08X\n", NV_PGSP_QUEUE_HEAD(1));

    TEST("NV_PGSP_QUEUE_TAIL(1) == 0x00110C88",
         NV_PGSP_QUEUE_TAIL(1) == 0x00110C88);
    printf("    NV_PGSP_QUEUE_TAIL(1) = 0x%08X\n", NV_PGSP_QUEUE_TAIL(1));

    TEST("NV_FUSE_OPT_FPF_GSP_UCODE1_VERSION == 0x008241C0",
         NV_FUSE_OPT_FPF_GSP_UCODE1_VERSION == 0x008241C0);
}

// ========================================================================
// Test 5: Ada Lovelace Specific Constants
// ========================================================================
void testAdaConstants() {
    printf("\n=== Test 5: Ada Lovelace Constants ===\n");

    TEST("ADA_FALCON_DESC_VERSION == 0x01",
         ADA_FALCON_DESC_VERSION == 0x01);

    TEST("ADA_FALCON_DESC_TYPE_FWSEC == 0x0010",
         ADA_FALCON_DESC_TYPE_FWSEC == 0x0010);

    TEST("ADA_FALCON_DESC_HEADER_SIZE == 0x0020",
         ADA_FALCON_DESC_HEADER_SIZE == 0x0020);

    TEST("FWSEC_FRTS_IMEM_CODE_SIZE == 0x0AE0",
         FWSEC_FRTS_IMEM_CODE_SIZE == 0x0AE0);

    TEST("FWSEC_FRTS_INTERFACE_OFFSET == 0x0D2C",
         FWSEC_FRTS_INTERFACE_OFFSET == 0x0D2C);

    TEST("FWSEC_SB_IMEM_CODE_SIZE == 0x09CC",
         FWSEC_SB_IMEM_CODE_SIZE == 0x09CC);

    TEST("FWSEC_SB_INTERFACE_OFFSET == 0x0C18",
         FWSEC_SB_INTERFACE_OFFSET == 0x0C18);

    TEST("BCRT30_RSA3K_SIG_SIZE == 384",
         BCRT30_RSA3K_SIG_SIZE == 384);
}

// ========================================================================
// Test 6: RPC Message Layout (mock message)
// ========================================================================
void testRpcMessageLayout() {
    printf("\n=== Test 6: RPC Message Layout ===\n");

    NvRpcMessageHeader hdr;
    memset(&hdr, 0, sizeof(hdr));

    hdr.signature     = NV_VGPU_MSG_SIGNATURE_VALID;
    hdr.headerVersion = (3 << 24) | 0x01;
    hdr.rpcResult     = 0;
    hdr.rpcResultPriv = 0;
    hdr.function      = NV_VGPU_MSG_FUNCTION_GSP_SET_SYSTEM_INFO;
    hdr.length        = sizeof(NvRpcMessageHeader) + sizeof(GspSystemInfo);

    // Verify signature is at the very start of the struct
    uint32_t firstWord;
    memcpy(&firstWord, &hdr, sizeof(uint32_t));
    TEST("Signature at start of message",
         firstWord == NV_VGPU_MSG_SIGNATURE_VALID);
    printf("    First 4 bytes of header: 0x%08X\n", firstWord);

    // Verify the fields we set
    TEST("signature field correct",
         hdr.signature == 0x43505256);
    TEST("headerVersion field correct",
         hdr.headerVersion == ((3 << 24) | 0x01));
    TEST("function field correct",
         hdr.function == 0x15);
    TEST("length field includes header + GspSystemInfo",
         hdr.length == sizeof(NvRpcMessageHeader) + sizeof(GspSystemInfo));
    printf("    Message length: %u (header=%zu + payload=%zu)\n",
           hdr.length, sizeof(NvRpcMessageHeader), sizeof(GspSystemInfo));

    // Verify memory layout: read function from raw bytes at offset 16
    uint8_t* raw = reinterpret_cast<uint8_t*>(&hdr);
    uint32_t funcFromRaw;
    memcpy(&funcFromRaw, raw + 16, sizeof(uint32_t));
    TEST("Function at byte offset 16 in raw memory",
         funcFromRaw == NV_VGPU_MSG_FUNCTION_GSP_SET_SYSTEM_INFO);
    printf("    Raw bytes [16..19]: 0x%08X (function)\n", funcFromRaw);

    // Verify length from raw bytes at offset 20
    uint32_t lenFromRaw;
    memcpy(&lenFromRaw, raw + 20, sizeof(uint32_t));
    TEST("Length at byte offset 20 in raw memory",
         lenFromRaw == hdr.length);
    printf("    Raw bytes [20..23]: 0x%08X (length)\n", lenFromRaw);
}

// ========================================================================
// Main
// ========================================================================
int main() {
    printf("NVDAAL RPC Struct & Constant Validation Test\n");

    testStructSizes();
    testFieldOffsets();
    testRpcConstants();
    testRegisterOffsets();
    testAdaConstants();
    testRpcMessageLayout();

    printf("\n========================================\n");
    printf("Results: %d passed, %d failed\n", testsPassed, testsFailed);
    printf("========================================\n");

    return testsFailed > 0 ? 1 : 0;
}
