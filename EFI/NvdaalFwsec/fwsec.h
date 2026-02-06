/**
 * @file fwsec.h
 * @brief FWSEC-FRTS definitions for NVIDIA Ada Lovelace GPUs
 *
 * Implements FWSEC (Firmware Security) for WPR2 configuration.
 * Based on NVIDIA open-gpu-kernel-modules implementation:
 *   - src/nvidia/src/kernel/gpu/gsp/arch/turing/kernel_gsp_frts_tu102.c
 *   - src/nvidia/src/kernel/gpu/gsp/kernel_gsp_fwsec.c
 *
 * Copyright (c) 2024-2025 Gabriel Maia / NVDAAL Project
 * SPDX-License-Identifier: MIT
 */

#ifndef NVDAAL_FWSEC_H_
#define NVDAAL_FWSEC_H_

#include <Uefi.h>
// Note: Not including vbios.h to avoid typedef conflicts

//==============================================================================
// Constants - From NVIDIA open-gpu-kernel-modules
//==============================================================================

// FWSEC Commands (DMEMMAPPER init_cmd values)
#define FWSEC_CMD_FRTS                      0x15
#define FWSEC_CMD_SB                        0x19

// FRTS Region Constants
#define FRTS_SIZE_1MB_IN_4K                 0x100   // 1MB in 4K pages
#define FRTS_REGION_MEDIA_FB                2       // Framebuffer media type

// Signature Size (RSA-3K, BCRT30)
#define RSA3K_SIGNATURE_SIZE                384

// Fuse Version Register Base
#define NV_FUSE_OPT_FPF_GSP_UCODE1_VERSION  0x008241C0

// Application Interface Constants
#define APPIF_ENTRY_ID_DMEMMAPPER           4
#define DMEMMAPPER_SIGNATURE                0x50414D44  // "DMAP" little-endian

// Error Scratch Registers
#define NV_PBUS_VBIOS_SCRATCH_0E            0x0000109C
#define NV_PBUS_VBIOS_SCRATCH_15            0x000010B8

// WPR2 Address Alignment (12 bits = 4KB pages)
#define WPR2_ADDR_ALIGNMENT                 12

// Falcon Ucode Descriptor Versions
#define FALCON_UCODE_DESC_VERSION_V2        0x02
#define FALCON_UCODE_DESC_VERSION_V3        0x03
#define FALCON_UCODE_DESC_V3_SIZE           44

// VDesc field masks
#define VDESC_FLAGS_VERSION_BIT             0x00000001
#define VDESC_VERSION_SHIFT                 8
#define VDESC_VERSION_MASK                  0x0000FF00
#define VDESC_SIZE_SHIFT                    16
#define VDESC_SIZE_MASK                     0xFFFF0000

//==============================================================================
// FWSEC Command Structures - Matching NVIDIA Driver Exactly
//==============================================================================

#pragma pack(push, 1)

/**
 * FWSECLIC_READ_VBIOS_DESC
 * From kernel_gsp_frts_tu102.c line 104-112
 */
typedef struct {
    UINT32  Version;            // = 1
    UINT32  Size;               // = sizeof(this) = 24
    UINT64  GfwImageOffset;     // = 0 for FRTS
    UINT32  GfwImageSize;       // = 0 for FRTS
    UINT32  Flags;              // = FWSECLIC_READ_VBIOS_STRUCT_FLAGS = 2
} FWSEC_READ_VBIOS_DESC;

#define FWSEC_READ_VBIOS_STRUCT_FLAGS   2

/**
 * FWSECLIC_FRTS_REGION_DESC
 * From kernel_gsp_frts_tu102.c line 114-123
 */
typedef struct {
    UINT32  Version;            // = 1
    UINT32  Size;               // = sizeof(this) = 20
    UINT32  FrtsOffset4K;       // = frtsOffset >> 12
    UINT32  FrtsSize4K;         // = 0x100 (1MB in 4K pages)
    UINT32  MediaType;          // = FWSECLIC_FRTS_REGION_MEDIA_FB = 2
} FWSEC_FRTS_REGION_DESC;

/**
 * FWSECLIC_FRTS_CMD - Complete FRTS command
 * From kernel_gsp_frts_tu102.c line 127-131
 * This is written to DMEMMAPPER cmd_in_buffer
 */
typedef struct {
    FWSEC_READ_VBIOS_DESC   ReadVbiosDesc;
    FWSEC_FRTS_REGION_DESC  FrtsRegionDesc;
} FWSEC_FRTS_CMD;

/**
 * FALCON_APPLICATION_INTERFACE_HEADER_V1
 * From kernel_gsp_frts_tu102.c line 64-70
 */
typedef struct {
    UINT8   Version;            // = 1
    UINT8   HeaderSize;         // Size of header
    UINT8   EntrySize;          // Size of each entry
    UINT8   EntryCount;         // Number of entries
} FALCON_APPIF_HEADER;

/**
 * FALCON_APPLICATION_INTERFACE_ENTRY_V1
 * From kernel_gsp_frts_tu102.c line 72-76
 */
typedef struct {
    UINT32  Id;                 // Entry ID (4 = DMEMMAPPER)
    UINT32  DmemOffset;         // Offset in DMEM
} FALCON_APPIF_ENTRY;

/**
 * FALCON_APPLICATION_INTERFACE_DMEM_MAPPER_V3
 * From kernel_gsp_frts_tu102.c line 80-99
 */
typedef struct {
    UINT32  Signature;          // = "DMAP" (0x50414D44)
    UINT16  Version;
    UINT16  Size;
    UINT32  CmdInBufferOffset;  // Offset to command input
    UINT32  CmdInBufferSize;
    UINT32  CmdOutBufferOffset;
    UINT32  CmdOutBufferSize;
    UINT32  NvfImgDataBufferOffset;
    UINT32  NvfImgDataBufferSize;
    UINT32  PrintfBufferHdr;
    UINT32  UcodeBuildTimeStamp;
    UINT32  UcodeSignature;
    UINT32  InitCmd;            // Command to execute (patched to 0x15)
    UINT32  UcodeFeature;
    UINT32  UcodeCmdMask0;
    UINT32  UcodeCmdMask1;
    UINT32  MultiTgtTbl;
} FALCON_DMEMMAPPER;

/**
 * FALCON_UCODE_DESC_V3 - Heavy Secure Ucode Descriptor
 * From kernel_gsp_fwsec.c line 170-185
 */
typedef struct {
    UINT32  VDesc;              // Version + Flags + Size
    UINT32  StoredSize;
    UINT32  PKCDataOffset;      // Where to patch signature in DMEM
    UINT32  InterfaceOffset;    // Where DMEMMAPPER is in DMEM
    UINT32  IMEMPhysBase;
    UINT32  IMEMLoadSize;
    UINT32  IMEMVirtBase;
    UINT32  DMEMPhysBase;
    UINT32  DMEMLoadSize;
    UINT16  EngineIdMask;
    UINT8   UcodeId;            // For fuse version lookup
    UINT8   SignatureCount;
    UINT16  SignatureVersions;  // Bitmask of available versions
    UINT16  Reserved;
} FALCON_UCODE_DESC_V3;

#pragma pack(pop)

//==============================================================================
// FWSEC Context
//==============================================================================

typedef struct {
    // Source VBIOS
    UINT8                   *VbiosData;
    UINTN                   VbiosSize;
    UINT32                  ExpansionRomOffset;

    // Ucode descriptor
    FALCON_UCODE_DESC_V3    UcodeDesc;
    UINT32                  UcodeDescOffset;
    UINT32                  UcodeDescSize;

    // IMEM/DMEM data (copied from VBIOS)
    UINT8                   *ImemData;
    UINT32                  ImemSize;
    UINT8                   *DmemData;      // Working copy for patching
    UINT32                  DmemSize;

    // Signatures (after descriptor in VBIOS)
    UINT8                   *Signatures;
    UINT32                  SignaturesTotalSize;

    // Selected signature
    UINT8                   FuseVersion;
    UINT32                  SelectedSigOffset;

    // FRTS target offset
    UINT64                  FrtsOffset;

    // GPU BAR0
    UINT32                  Bar0;
} FWSEC_CONTEXT;

//==============================================================================
// Function Prototypes
//==============================================================================

/**
 * Parse VBIOS and extract FWSEC ucode
 * Based on kgspParseFwsecUcodeFromVbiosImg_IMPL
 */
EFI_STATUS
FwsecParseFromVbios (
    OUT FWSEC_CONTEXT   *Context,
    IN  UINT32          Bar0,
    IN  UINT8           *VbiosData,
    IN  UINTN           VbiosSize
    );

/**
 * Read fuse version for ucode ID
 * Based on kgspReadUcodeFuseVersion_HAL
 */
UINT32
FwsecReadFuseVersion (
    IN  UINT32          Bar0,
    IN  UINT8           UcodeId
    );

/**
 * Select signature based on fuse version
 * Based on NVIDIA's signature selection algorithm (lines 357-378)
 */
EFI_STATUS
FwsecSelectSignature (
    IN OUT FWSEC_CONTEXT *Context
    );

/**
 * Patch signature into DMEM at PKCDataOffset
 * Based on lines 396-397 of kernel_gsp_frts_tu102.c
 */
EFI_STATUS
FwsecPatchSignature (
    IN OUT FWSEC_CONTEXT *Context
    );

/**
 * Patch FRTS command into DMEMMAPPER
 * Based on s_vbiosPatchInterfaceData
 */
EFI_STATUS
FwsecPatchFrtsCmd (
    IN OUT FWSEC_CONTEXT *Context,
    IN     UINT64        FrtsOffset
    );

/**
 * Load IMEM/DMEM into Falcon
 */
EFI_STATUS
FwsecLoadUcode (
    IN  FWSEC_CONTEXT   *Context
    );

/**
 * Execute Falcon and wait for completion
 */
EFI_STATUS
FwsecExecute (
    IN  FWSEC_CONTEXT   *Context
    );

/**
 * Verify WPR2 configuration
 * Based on lines 506-525 of kernel_gsp_frts_tu102.c
 */
EFI_STATUS
FwsecVerifyWpr2 (
    IN  FWSEC_CONTEXT   *Context
    );

/**
 * Complete FWSEC-FRTS execution sequence
 * Main entry point
 */
EFI_STATUS
FwsecExecuteFrts (
    IN  UINT32          Bar0,
    IN  UINT8           *VbiosData,
    IN  UINTN           VbiosSize,
    IN  UINT64          FrtsOffset
    );

/**
 * Free context resources
 */
VOID
FwsecFreeContext (
    IN OUT FWSEC_CONTEXT *Context
    );

//==============================================================================
// GPU Register Access Helpers
//==============================================================================

/**
 * Read 32-bit GPU register
 */
static inline UINT32
GpuRead32 (
    IN  UINT32  Bar0,
    IN  UINT32  Offset
    )
{
    volatile UINT32 *Reg = (volatile UINT32 *)(UINTN)(Bar0 + Offset);
    return *Reg;
}

/**
 * Write 32-bit GPU register
 */
static inline VOID
GpuWrite32 (
    IN  UINT32  Bar0,
    IN  UINT32  Offset,
    IN  UINT32  Value
    )
{
    volatile UINT32 *Reg = (volatile UINT32 *)(UINTN)(Bar0 + Offset);
    *Reg = Value;
}

#endif // NVDAAL_FWSEC_H_
