/*
 * NVDAALRegs.h - NVIDIA GPU Register Definitions
 *
 * Based on:
 * - TinyGPU/tinygrad (https://github.com/tinygrad/tinygrad)
 * - NVIDIA open-gpu-kernel-modules
 * - Nouveau project
 *
 * For Ada Lovelace (AD102) - RTX 4090
 */

#ifndef NVDAAL_REGS_H
#define NVDAAL_REGS_H

#include <stdint.h>

// ============================================================================
// Chip Identification
// ============================================================================

#define NV_PMC_BOOT_0                     0x00000000
#define NV_PMC_BOOT_0_ARCHITECTURE        24:20  // Bits 24:20
#define NV_PMC_BOOT_0_IMPLEMENTATION      23:20

#define NV_PMC_BOOT_42                    0x00000188

// Architecture values
#define NV_CHIP_ARCH_AMPERE               0x17   // GA1xx
#define NV_CHIP_ARCH_ADA                  0x19   // AD1xx
#define NV_CHIP_ARCH_BLACKWELL            0x1B   // GB2xx

// ============================================================================
// PMC (Power Management Controller)
// ============================================================================

#define NV_PMC_ENABLE                     0x00000200
#define NV_PMC_DEVICE_ENABLE              0x00000600
#define NV_PMC_INTR_EN_0                  0x00000140

// ============================================================================
// PBUS (Bus Control)
// ============================================================================

#define NV_PBUS_PRI_TIMEOUT_SAVE_0        0x00001460
#define NV_PBUS_VBIOS_SCRATCH             0x00001400

// ============================================================================
// PFB (Framebuffer / Memory Controller)
// ============================================================================

#define NV_PFB_PRI_MMU_CTRL               0x00100C80
#define NV_PFB_PRI_MMU_WPR2_ADDR_LO       0x001FA820
#define NV_PFB_PRI_MMU_WPR2_ADDR_HI       0x001FA824

// WPR2 (Write Protected Region 2) status check
#define NV_PFB_WPR2_ENABLED(val)          (((val) >> 31) & 1)

// ============================================================================
// FALCON (Secure Co-processor)
// ============================================================================

#define NV_PFALCON_FALCON_BASE            0x00110000

// Generic FALCON offsets (add to base)
#define FALCON_IRQSSET                    0x0000
#define FALCON_IRQSCLR                    0x0004
#define FALCON_IRQSTAT                    0x0008
#define FALCON_IRQMSET                    0x0010
#define FALCON_IRQMCLR                    0x0014
#define FALCON_IRQDEST                    0x001C
#define FALCON_MAILBOX0                   0x0040
#define FALCON_MAILBOX1                   0x0044
#define FALCON_ITFEN                      0x0048
#define FALCON_IDLESTATE                  0x004C
#define FALCON_CURCTX                     0x0050
#define FALCON_NXTCTX                     0x0054
#define FALCON_SCRATCH0                   0x0080
#define FALCON_SCRATCH1                   0x0084
#define FALCON_CPUCTL                     0x0100
#define FALCON_BOOTVEC                    0x0104
#define FALCON_HWCFG                      0x0108
#define FALCON_DMACTL                     0x010C
#define FALCON_DMATRFBASE                 0x0110
#define FALCON_DMATRFMOFFS                0x0114
#define FALCON_DMATRFCMD                  0x0118
#define FALCON_DMATRFFBOFFS               0x011C
#define FALCON_EXTERRADDR                 0x0168
#define FALCON_EXTERRSTAT                 0x016C
#define FALCON_ENGCTL                     0x01A4
#define FALCON_IMEMC(i)                   (0x0180 + (i) * 16)
#define FALCON_IMEMD(i)                   (0x0184 + (i) * 16)
#define FALCON_IMEMT(i)                   (0x0188 + (i) * 16)
#define FALCON_DMEMC(i)                   (0x01C0 + (i) * 8)
#define FALCON_DMEMD(i)                   (0x01C4 + (i) * 8)

// FALCON CPUCTL bits
#define FALCON_CPUCTL_STARTCPU            (1 << 1)
#define FALCON_CPUCTL_HALTED              (1 << 4)

// ============================================================================
// GSP (GPU System Processor) - RISC-V based
// ============================================================================

#define NV_PGSP_BASE                      0x00110000

#define NV_PGSP_FALCON_MAILBOX0           (NV_PGSP_BASE + FALCON_MAILBOX0)
#define NV_PGSP_FALCON_MAILBOX1           (NV_PGSP_BASE + FALCON_MAILBOX1)
#define NV_PGSP_FALCON_CPUCTL             (NV_PGSP_BASE + FALCON_CPUCTL)

// GSP Queue registers
#define NV_PGSP_QUEUE_HEAD(i)             (0x00110C00 + (i) * 8)
#define NV_PGSP_QUEUE_TAIL(i)             (0x00110C80 + (i) * 8)

// Queue indices
#define GSP_CMDQ_IDX                      0  // Command queue
#define GSP_MSGQ_IDX                      1  // Message/status queue

// ============================================================================
// RISC-V Control (GSP Core on Ada+)
// ============================================================================

#define NV_PRISCV_RISCV_BASE              0x00110000

#define NV_PRISCV_RISCV_CPUCTL            0x00110388
#define NV_PRISCV_RISCV_BCR_CTRL          0x00110668
#define NV_PRISCV_RISCV_BCR_DMEM_ADDR     0x0011066C
#define NV_PRISCV_RISCV_BR_RETCODE        0x00110400
#define NV_PRISCV_RISCV_CORE_HALT         0x00110544

// BCR_CTRL bits
#define NV_PRISCV_RISCV_BCR_CTRL_VALID    (1 << 0)
#define NV_PRISCV_RISCV_BCR_CTRL_DMAADDR  0x1FFFFFFF

// CPUCTL bits
#define NV_PRISCV_CPUCTL_HALTED           (1 << 4)
#define NV_PRISCV_CPUCTL_ACTIVE           (1 << 7)
#define NV_PRISCV_CPUCTL_START            (1 << 1)

// ============================================================================
// SEC2 (Security Engine 2)
// ============================================================================

#define NV_PSEC_BASE                      0x00840000
#define NV_PSEC_FALCON_CPUCTL             (NV_PSEC_BASE + FALCON_CPUCTL)
#define NV_PSEC_FALCON_MAILBOX0           (NV_PSEC_BASE + FALCON_MAILBOX0)
#define NV_PSEC_FALCON_MAILBOX1           (NV_PSEC_BASE + FALCON_MAILBOX1)

// ============================================================================
// Compute Engine (CE)
// ============================================================================

#define NV_PCE_BASE                       0x00104000
#define NV_PCE_FALCON_MAILBOX0            (NV_PCE_BASE + FALCON_MAILBOX0)
#define NV_PCE_INTR_EN                    (NV_PCE_BASE + 0x100)

// ============================================================================
// Timer
// ============================================================================

#define NV_PTIMER_TIME_0                  0x00009400  // Low 32 bits
#define NV_PTIMER_TIME_1                  0x00009410  // High 32 bits

// ============================================================================
// RPC Message Definitions
// ============================================================================

#define NV_VGPU_MSG_SIGNATURE_VALID       0x43505256  // "VRPC" in little endian

// RPC Function codes
#define NV_VGPU_MSG_FUNCTION_GSP_SET_SYSTEM_INFO    0x15
#define NV_VGPU_MSG_FUNCTION_SET_REGISTRY           0x16
#define NV_VGPU_MSG_FUNCTION_GSP_RM_ALLOC           0x24
#define NV_VGPU_MSG_FUNCTION_GSP_RM_CONTROL         0x25

// RPC Events
#define NV_VGPU_MSG_EVENT_GSP_INIT_DONE             0x52

// ============================================================================
// RPC Structures
// ============================================================================

#pragma pack(push, 1)

// RPC Message Header
typedef struct {
    uint32_t signature;      // NV_VGPU_MSG_SIGNATURE_VALID
    uint32_t headerVersion;  // (3 << 24) | minor
    uint32_t rpcResult;      // Result code
    uint32_t rpcResultPriv;  // Private result
    uint32_t function;       // Function code
    uint32_t length;         // Total message length
} NvRpcMessageHeader;

// GSP Queue Message Element
typedef struct {
    uint32_t checkSum;       // CRC32 of message
    uint32_t seqNum;         // Sequence number
    uint32_t elemCount;      // Number of 4KB elements
    uint32_t reserved;
    uint8_t  data[];         // Payload
} GspQueueElement;

// WPR Metadata for GSP boot
typedef struct {
    uint32_t magic;                    // 0x57505232 "WPR2"

    uint32_t bootloaderCodeOffset;
    uint32_t bootloaderCodeSize;
    uint32_t bootloaderDataOffset;
    uint32_t bootloaderDataSize;
    uint32_t bootloaderManifestOffset;

    uint64_t sysmemAddrOfRadix3Elf;
    uint64_t sizeOfRadix3Elf;

    uint64_t sysmemAddrOfBootloader;
    uint64_t sizeOfBootloader;

    uint64_t sysmemAddrOfSignature;
    uint64_t sizeOfSignature;

    uint64_t gspFwHeapVirtAddr;
    uint64_t gspFwHeapSize;            // ~129MB (0x8100000)
    uint64_t gspFwOffset;

    uint64_t bootBinVirtAddr;
    uint64_t bootBinSize;

    uint64_t frtsOffset;
    uint64_t frtsSize;                 // 1MB (0x100000)

    uint64_t gspFwWprEnd;

    uint32_t fwHeapEnabled;
    uint32_t partitionRpc;
} GspFwWprMeta;

// Libos init arguments (passed to GSP)
typedef struct {
    uint64_t dmemAddr;        // DMEM address
    uint64_t gspFwWprMeta;    // Physical address of WPR metadata
    uint64_t cmdQueueOffset;  // Offset of command queue in DMEM
    uint64_t statQueueOffset; // Offset of status queue in DMEM
    uint64_t queueSize;       // Size of each queue
} GspLibosInitArgs;

// System info for GSP
typedef struct {
    uint64_t gpuPhysAddr;
    uint64_t gpuPhysSize;
    uint64_t fbPhysAddr;
    uint64_t fbPhysSize;
    uint32_t pciDomain;
    uint32_t pciBus;
    uint32_t pciDevice;
    uint32_t pciFunction;
    uint32_t pciVendorId;
    uint32_t pciDeviceId;
    uint32_t pciSubVendorId;
    uint32_t pciSubDeviceId;
    uint32_t pciRevisionId;
    // ... more fields
} GspSystemInfo;

#pragma pack(pop)

// ============================================================================
// Helper Macros
// ============================================================================

// Extract bits from value
#define NV_DRF_VAL(drf, val)  (((val) >> (drf & 0xFF)) & ((1 << ((drf >> 8) - (drf & 0xFF) + 1)) - 1))

// Create mask for bits
#define NV_MASK(hi, lo)       (((1 << ((hi) - (lo) + 1)) - 1) << (lo))

// Memory barrier
#define NV_MEMORY_BARRIER()   __asm__ __volatile__ ("mfence" ::: "memory")

// ============================================================================
// GSP Firmware ELF Sections
// ============================================================================

#define GSP_FW_SECTION_IMAGE          ".fwimage"
#define GSP_FW_SECTION_SIG_AD10X      ".fwsignature_ad10x"
#define GSP_FW_SECTION_SIG_GA10X      ".fwsignature_ga10x"

// Radix3 page table constants
#define GSP_RADIX3_LEVELS             4  // 0, 1, 2, 3

// ============================================================================
// Compute Class Definitions
// ============================================================================

#define ADA_COMPUTE_A                 0xC9C0  // AD102 compute class
#define KEPLER_CHANNEL_GPFIFO_A       0xA06F

// RM Object Classes (for GSP_RM_ALLOC)
#define NV01_ROOT                     0x00000000
#define NV01_ROOT_CLIENT              0x00000041
#define NV01_DEVICE_0                 0x00000080
#define NV20_SUBDEVICE_0              0x00002080
#define FERMI_VASPACE_A               0x000090F1
#define GF100_CHANNEL_GPFIFO          0x0000906F

// ============================================================================
// GSP / RM Classes & Structures
// ============================================================================

//
// NVIDIA Class IDs (Architecture Specific)
//
#define NV01_ROOT                       0x00000000
// #define NV01_ROOT_CLIENT                0x00000000 // Host Client (Already defined below or handle appropriately)
// NV01_ROOT_CLIENT is defined as 0x00000041 in older headers, but 0x00 for RM handle usage in some contexts.
// We will use the specific value for RM alloc:
#undef NV01_ROOT_CLIENT
#define NV01_ROOT_CLIENT                0x00000000 
#define NV01_EVENT_OS_EVENT             0x00000079

#define AD102_COMPUTE_A                 0x0000C9C0 // Ada Compute

#define FERMI_TWOD_A                    0x0000902D
#define FERMI_VASPACE_A                 0x000090F1
#define FERMI_CONTEXT_SHARE_A           0x00009067

#define GF100_SUBDEVICE_FULL            0x00002080

#define NV_MEMORY_FABRIC                0x000000F8 // Memory Fabric
#define NV01_MEMORY_SYSTEM              0x0000003E // System Memory (GTT)
#define NV01_MEMORY_LOCAL_USER          0x00000040 // VRAM

#define AMPERE_CHANNEL_GPFIFO_A         0x0000C56F // GPFIFO (Ampere+)
#define ADA_CHANNEL_GPFIFO_A            0x0000C96F // GPFIFO (Ada)

#define NV_CONF_COMPUTE_CAPABILITY      0x0000C7C0

//
// RPC Message IDs (VGPU)
//
#define NV_VGPU_MSG_FUNCTION_GSP_RM_ALLOC      0x24
#define NV_VGPU_MSG_FUNCTION_GSP_RM_CONTROL    0x25
#define NV_VGPU_MSG_FUNCTION_GSP_RM_FREE       0x26

//
// RM Handles (Arbitrary software handles used by driver to ID objects)
//
#define NV01_NULL_OBJECT                0x00000000
#define NV01_ROOT_HANDLE                0x00000000

//
// RM Allocation Parameters (Generic Header)
//
struct NvGspAllocParams {
    uint32_t hClient;    // Handle of the Client
    uint32_t hParent;    // Handle of the Parent Object
    uint32_t hObject;    // Handle of the New Object (to be created)
    uint32_t hClass;     // Class ID of the New Object
    uint32_t status;     // Out: Status
};

//
// Specific Allocation Structures
//

// NV01_ROOT_CLIENT
struct Nv01RootClientParams {
    uint32_t reserved;
};

// FERMI_VASPACE_A
struct NvFermiVASpaceParams {
    uint32_t index;       // usually 0
    uint32_t flags;       // allocation flags
    uint64_t vaSize;      // Virtual Address space size
    uint64_t vaStart;     // Start address
    uint64_t vaBase;      // Base address
    uint64_t vaLimit;     // Limit address
    uint32_t bigPageSize; // e.g. 64KB
    uint32_t reserved;
};

// NV01_MEMORY_SYSTEM / LOCAL_USER
struct NvMemoryAllocParams {
    uint32_t type;        // Page type/kind
    uint32_t flags;       // Allocation flags
    uint32_t attr;        // Caching attributes
    uint32_t format;      // Pixel format (if applicable)
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint64_t size;
    uint64_t alignment;
    uint64_t offset;      // Out: physical/virtual offset
    uint64_t limit;
    uint64_t address;     // Fixed address request
};

//
// RM Control (IOCTL) Structure
//
struct NvGspControlParams {
    uint32_t hClient;
    uint32_t hObject;
    uint32_t cmd;        // Control Command ID
    uint32_t flags;
    uint32_t status;     // Out: Status
    uint32_t paramsSize; // Size of inline parameters
    // followed by params...
};

//
// Channel Allocation Parameters (ADA_CHANNEL_GPFIFO_A)
//
struct NvChannelAllocParams {
    uint32_t ampMode;      // Ampere+ Mode
    uint32_t engineType;   // NV2080_ENGINE_TYPE_*
    uint32_t gpFifoOffset; // Offset of GPFIFO in user memory
    uint32_t gpFifoEntries;// Number of entries
    uint32_t flags;
    uint32_t hUserdMemory; // Handle to UserD memory
    uint64_t userdOffset;  // Offset within UserD memory
};

// Engine Types
#define NV2080_ENGINE_TYPE_GRAPHICS 0
#define NV2080_ENGINE_TYPE_COMPUTE  1
#define NV2080_ENGINE_TYPE_COPY     2

#endif // NVDAAL_REGS_H
