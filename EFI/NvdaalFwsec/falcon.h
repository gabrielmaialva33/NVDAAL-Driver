/**
 * @file falcon.h
 * @brief Falcon microcontroller definitions for Ada Lovelace GPUs
 *
 * Based on Linux nova-core driver (falcon.rs, regs.rs)
 * Reference: https://docs.kernel.org/gpu/nova/core/falcon.html
 */

#ifndef NVDAAL_FALCON_H
#define NVDAAL_FALCON_H

#include <Uefi.h>

//
// Falcon Base Addresses (offsets from BAR0)
//
#define FALCON_GSP_BASE             0x110000    // GSP Falcon base
#define FALCON_SEC2_BASE            0x840000    // SEC2 Falcon base
#define FALCON_PMU_BASE             0x10A000    // PMU Falcon base

//
// Falcon Register Offsets (from falcon base)
//
#define FALCON_IRQSSET              0x0000      // IRQ set
#define FALCON_IRQSCLR              0x0004      // IRQ clear
#define FALCON_IRQSTAT              0x0008      // IRQ status
#define FALCON_IRQMSET              0x0010      // IRQ mask set
#define FALCON_IRQMCLR              0x0014      // IRQ mask clear
#define FALCON_IRQMASK              0x0018      // IRQ mask
#define FALCON_IRQDEST              0x001C      // IRQ destination

#define FALCON_OS                   0x0080      // OS register
#define FALCON_CPUCTL               0x0100      // CPU control
#define FALCON_BOOTVEC              0x0104      // Boot vector
#define FALCON_HWCFG                0x0108      // Hardware config
#define FALCON_HWCFG1               0x012C      // Hardware config 1
#define FALCON_HWCFG2               0x0F98      // Hardware config 2 (scrub status)
#define FALCON_CPUCTL_ALIAS         0x0130      // CPU control alias
#define FALCON_MAILBOX0             0x0040      // Mailbox 0
#define FALCON_MAILBOX1             0x0044      // Mailbox 1

#define FALCON_ITFEN                0x0048      // Interface enable
#define FALCON_IDLESTATE            0x004C      // Idle state

#define FALCON_CURCTX               0x0050      // Current context
#define FALCON_NXTCTX               0x0054      // Next context
#define FALCON_SCRATCH0             0x0058      // Scratch register 0
#define FALCON_SCRATCH1             0x005C      // Scratch register 1

// IMEM/DMEM access
#define FALCON_IMEMC(i)             (0x0180 + (i) * 16)  // IMEM control
#define FALCON_IMEMD(i)             (0x0184 + (i) * 16)  // IMEM data
#define FALCON_IMEMT(i)             (0x0188 + (i) * 16)  // IMEM tag
#define FALCON_DMEMC(i)             (0x01C0 + (i) * 8)   // DMEM control
#define FALCON_DMEMD(i)             (0x01C4 + (i) * 8)   // DMEM data

// DMA registers
#define FALCON_DMACTL               0x010C      // DMA control
#define FALCON_DMATRFBASE           0x0110      // DMA transfer base (low)
#define FALCON_DMATRFBASE1          0x0128      // DMA transfer base (high)
#define FALCON_DMATRFMOFFS          0x0114      // DMA transfer mem offset
#define FALCON_DMATRFFBOFFS         0x0118      // DMA transfer FB offset
#define FALCON_DMATRFCMD            0x011C      // DMA transfer command
#define FALCON_DMATRFSTAT           0x0120      // DMA transfer status (unofficial)

// BROM (Boot ROM) registers
#define FALCON_BROM_ENGCTL          0x00A4      // BROM engine control
#define FALCON_BROM_PARAM           0x00AC      // BROM parameter
#define FALCON_BROM_ADDR            0x00B0      // BROM address
#define FALCON_BROM_DATA            0x00B4      // BROM data

// BCR (Boot Control) registers
#define FALCON_BCR_CTRL             0x0F54      // BCR control

// FBIF (Framebuffer Interface) registers
#define FALCON_FBIF_CTL             0x0624      // FBIF control
#define FALCON_FBIF_TRANSCFG        0x0600      // FBIF transfer config

//
// CPUCTL Register Bits
//
#define FALCON_CPUCTL_STARTCPU      (1 << 1)    // Start CPU
#define FALCON_CPUCTL_HALTED        (1 << 4)    // CPU halted
#define FALCON_CPUCTL_STOPPED       (1 << 5)    // CPU stopped
#define FALCON_CPUCTL_ALIAS_EN      (1 << 6)    // Alias enable

//
// HWCFG2 Register Bits
//
#define FALCON_HWCFG2_RISCV         (1 << 0)    // RISC-V capable
#define FALCON_HWCFG2_MEM_SCRUBBING (1 << 5)    // Memory scrubbing in progress

//
// BCR_CTRL Register Values
//
#define FALCON_BCR_CTRL_CORE_SELECT 0x00000001  // Select Falcon core
#define FALCON_BCR_CTRL_RESET       0x00000110  // Reset state

//
// DMACTL Register Bits
//
#define FALCON_DMACTL_ENABLE        (1 << 0)    // Enable DMA engine
#define FALCON_DMACTL_DMEM_SCRUB    (1 << 1)    // DMEM scrub
#define FALCON_DMACTL_IMEM_SCRUB    (1 << 2)    // IMEM scrub

//
// DMATRFCMD Register Bits
//
#define FALCON_DMATRFCMD_IDLE       (1 << 1)    // DMA idle
#define FALCON_DMATRFCMD_SEC        (1 << 2)    // Secure mode
#define FALCON_DMATRFCMD_IMEM       (1 << 4)    // Target IMEM (vs DMEM)
#define FALCON_DMATRFCMD_SIZE_SHIFT 8           // Size field shift
#define FALCON_DMATRFCMD_SIZE_256B  (6 << 8)    // 256 byte transfer

//
// ITFEN Register Bits
//
#define FALCON_ITFEN_CTXEN          (1 << 0)    // Context enable
#define FALCON_ITFEN_MTHDEN         (1 << 1)    // Method enable
#define FALCON_ITFEN_FBIF           (1 << 2)    // FBIF enable

//
// IMEMC/DMEMC Register Bits
//
#define FALCON_MEMC_BLK_SHIFT       8           // Block number shift
#define FALCON_MEMC_AINCW           (1 << 24)   // Auto-increment on write
#define FALCON_MEMC_AINCR           (1 << 25)   // Auto-increment on read
#define FALCON_MEMC_SEC             (1 << 28)   // Secure access

//
// FBIF Target Types
//
#define FALCON_FBIF_TARGET_LOCAL_FB 0           // Local framebuffer
#define FALCON_FBIF_TARGET_COHERENT 1           // Coherent system memory
#define FALCON_FBIF_TARGET_NONCOHER 2           // Non-coherent system memory

//
// GPU Registers (from BAR0)
//

// PMC (Power Management Controller)
#define NV_PMC_BOOT_0               0x00000000  // Boot register
#define NV_PMC_ENABLE               0x00000200  // Engine enable

// PBUS
#define NV_PBUS_SW_SCRATCH_0E       0x00001438  // FRTS error code register

// PFB (Framebuffer)
#define NV_PFB_NISO_FLUSH_SYSMEM_ADDR       0x00100C10
#define NV_PFB_FBHUB_PCIE_FLUSH_SYSMEM_ADDR 0x00100C14
#define NV_PFB_PRI_MMU_WPR2_ADDR_LO         0x001FA824  // WPR2 low address
#define NV_PFB_PRI_MMU_WPR2_ADDR_HI         0x001FA828  // WPR2 high address
#define NV_USABLE_FB_SIZE_IN_MB             0x00100A10  // Usable FB size

// PDISP (Display)
#define NV_PDISP_VGA_WORKSPACE_BASE 0x00611188  // VGA workspace base

// GFW (GPU Firmware)
#define NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_0 0x00118234  // GFW boot progress

// Fuse registers (for signature selection)
#define NV_FUSE_OPT_FPF_FWSEC_DBG_DISABLE    0x00824100
#define NV_FUSE_OPT_SECURE_GSP_DEBUG_DISABLE 0x00824104

//
// PMC_BOOT_0 Fields
//
#define NV_PMC_BOOT_0_ARCH_SHIFT    20
#define NV_PMC_BOOT_0_ARCH_MASK     (0x1F << 20)
#define NV_PMC_BOOT_0_IMPL_SHIFT    0
#define NV_PMC_BOOT_0_IMPL_MASK     0xFF

// Architecture IDs
#define NV_ARCH_AD10X               0x92        // Ada Lovelace (RTX 40)
#define NV_ARCH_GA10X               0x8E        // Ampere (RTX 30)
#define NV_ARCH_TU10X               0x86        // Turing (RTX 20)

//
// GFW Boot Progress Values
//
#define GFW_BOOT_PROGRESS_COMPLETED 0xFF

//
// WPR2 Register Macros
//
#define WPR2_ADDR_LO_SHIFT          12
#define WPR2_ADDR_HI_SHIFT          12

//
// Falcon Memory Limits
//
#define FALCON_IMEM_MAX_SIZE        0x40000     // 256KB max IMEM
#define FALCON_DMEM_MAX_SIZE        0x10000     // 64KB max DMEM
#define FALCON_DMA_BLOCK_SIZE       256         // DMA block size

//
// Falcon State Structure
//
typedef struct {
    UINT32              Base;           // Falcon base address
    BOOLEAN             IsGsp;          // Is this the GSP falcon?
    BOOLEAN             IsRiscV;        // RISC-V capable?
    BOOLEAN             Halted;         // CPU halted?
    UINT32              MailBox0;       // Last MBOX0 value
    UINT32              MailBox1;       // Last MBOX1 value
} FALCON_STATE;

//
// Falcon Firmware Load Parameters
//
typedef struct {
    // IMEM parameters
    UINT32              ImemSrcStart;   // Source offset for IMEM
    UINT32              ImemDstStart;   // Destination in IMEM
    UINT32              ImemSize;       // IMEM size to load

    // DMEM parameters
    UINT32              DmemSrcStart;   // Source offset for DMEM
    UINT32              DmemDstStart;   // Destination in DMEM
    UINT32              DmemSize;       // DMEM size to load

    // Boot parameters
    UINT32              BootVector;     // Boot vector address
    BOOLEAN             UseBrom;        // Use BROM interface
} FALCON_LOAD_PARAMS;

//
// BROM Parameters (for Heavy Secure execution)
//
typedef struct {
    UINT32              PkcDataOffset;  // Offset to signature in DMA buffer
    UINT32              EngineIdMask;   // Engine ID mask
    UINT8               UcodeId;        // Ucode ID
} FALCON_BROM_PARAMS;

//
// Function Prototypes
//

/**
 * Initialize Falcon state
 */
EFI_STATUS
FalconInit (
    OUT FALCON_STATE    *State,
    IN  UINT32          Bar0,
    IN  UINT32          FalconBase
    );

/**
 * Reset Falcon CPU
 */
EFI_STATUS
FalconReset (
    IN  UINT32          Bar0,
    IN  FALCON_STATE    *State
    );

/**
 * Wait for memory scrub to complete
 */
EFI_STATUS
FalconWaitScrubDone (
    IN  UINT32          Bar0,
    IN  FALCON_STATE    *State,
    IN  UINTN           TimeoutUs
    );

/**
 * Select Falcon core (for dual-controller chips)
 */
EFI_STATUS
FalconSelectCore (
    IN  UINT32          Bar0,
    IN  FALCON_STATE    *State
    );

/**
 * Load firmware via PIO (Programmed I/O)
 */
EFI_STATUS
FalconLoadPio (
    IN  UINT32              Bar0,
    IN  FALCON_STATE        *State,
    IN  UINT8               *FwData,
    IN  FALCON_LOAD_PARAMS  *Params
    );

/**
 * Load firmware via DMA
 */
EFI_STATUS
FalconLoadDma (
    IN  UINT32              Bar0,
    IN  FALCON_STATE        *State,
    IN  EFI_PHYSICAL_ADDRESS DmaPhysAddr,
    IN  FALCON_LOAD_PARAMS  *Params
    );

/**
 * Configure FBIF for DMA
 */
EFI_STATUS
FalconConfigureFbif (
    IN  UINT32          Bar0,
    IN  FALCON_STATE    *State,
    IN  UINT8           Target
    );

/**
 * Start Falcon execution
 */
EFI_STATUS
FalconStart (
    IN  UINT32          Bar0,
    IN  FALCON_STATE    *State,
    IN  UINT32          BootVector
    );

/**
 * Start Falcon with BROM (Heavy Secure mode)
 */
EFI_STATUS
FalconStartBrom (
    IN  UINT32              Bar0,
    IN  FALCON_STATE        *State,
    IN  FALCON_BROM_PARAMS  *BromParams
    );

/**
 * Wait for Falcon to halt
 */
EFI_STATUS
FalconWaitHalt (
    IN  UINT32          Bar0,
    IN  FALCON_STATE    *State,
    IN  UINTN           TimeoutUs
    );

/**
 * Read mailbox values
 */
EFI_STATUS
FalconReadMailbox (
    IN  UINT32          Bar0,
    IN  FALCON_STATE    *State,
    OUT UINT32          *Mbox0,
    OUT UINT32          *Mbox1
    );

/**
 * Get signature fuse version
 */
EFI_STATUS
FalconGetSigFuseVersion (
    IN  UINT32          Bar0,
    IN  UINT32          EngineIdMask,
    IN  UINT8           UcodeId,
    OUT UINT32          *FuseVersion
    );

//
// GPU Helper Functions
//

/**
 * Wait for GFW boot to complete
 */
EFI_STATUS
GpuWaitGfwBoot (
    IN  UINT32          Bar0,
    IN  UINTN           TimeoutUs
    );

/**
 * Check if WPR2 is already configured
 */
BOOLEAN
GpuIsWpr2Configured (
    IN  UINT32          Bar0
    );

/**
 * Read WPR2 region
 */
EFI_STATUS
GpuReadWpr2 (
    IN  UINT32          Bar0,
    OUT UINT64          *Wpr2Lo,
    OUT UINT64          *Wpr2Hi
    );

/**
 * Get FRTS error code
 */
UINT16
GpuGetFrtsErrorCode (
    IN  UINT32          Bar0
    );

/**
 * Get usable framebuffer size
 */
UINT64
GpuGetUsableFbSize (
    IN  UINT32          Bar0
    );

/**
 * Get GPU architecture from PMC_BOOT_0
 */
UINT8
GpuGetArchitecture (
    IN  UINT32          Bar0
    );

//
// Inline Register Helpers
//

static inline UINT32
ReadReg32 (
    IN UINT32 Bar0,
    IN UINT32 Offset
    )
{
    volatile UINT32 *Reg = (volatile UINT32 *)(UINTN)(Bar0 + Offset);
    return *Reg;
}

static inline VOID
WriteReg32 (
    IN UINT32 Bar0,
    IN UINT32 Offset,
    IN UINT32 Value
    )
{
    volatile UINT32 *Reg = (volatile UINT32 *)(UINTN)(Bar0 + Offset);
    *Reg = Value;
}

static inline UINT32
FalconReadReg (
    IN UINT32 Bar0,
    IN UINT32 FalconBase,
    IN UINT32 Offset
    )
{
    return ReadReg32(Bar0, FalconBase + Offset);
}

static inline VOID
FalconWriteReg (
    IN UINT32 Bar0,
    IN UINT32 FalconBase,
    IN UINT32 Offset,
    IN UINT32 Value
    )
{
    WriteReg32(Bar0, FalconBase + Offset, Value);
}

#endif // NVDAAL_FALCON_H
