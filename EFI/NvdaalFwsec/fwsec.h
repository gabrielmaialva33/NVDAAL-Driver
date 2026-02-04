/**
 * @file fwsec.h
 * @brief FWSEC (Firmware Security) structures for Ada Lovelace GPUs
 *
 * Based on Linux nova-core driver (firmware/fwsec.rs)
 * Reference: https://docs.kernel.org/gpu/nova/core/fwsec.html
 */

#ifndef NVDAAL_FWSEC_H
#define NVDAAL_FWSEC_H

#include <Uefi.h>
#include "vbios.h"

//
// FWSEC Constants
//
#define FWSEC_RSA3K_SIG_SIZE        384     // RSA-3K signature size in bytes
#define FWSEC_DMEM_MAPPER_SIG       0x50414D44  // "DMAP" in LE
#define FWSEC_DMEM_MAPPER_VERSION   0x0003

//
// FWSEC Commands
//
#define FWSEC_CMD_FRTS              0x15    // FWSEC-FRTS: Setup WPR2
#define FWSEC_CMD_SB                0x1A    // Secure Boot
#define FWSEC_CMD_DEVINIT           0x01    // Device Init

//
// Application Interface IDs
//
#define NVFW_FALCON_APPIF_ID_DEVINIT        0x01
#define NVFW_FALCON_APPIF_ID_DMEMMAPPER     0x04
#define NVFW_FALCON_APPIF_ID_RECOVERY       0x05

//
// FRTS Error Codes (from NV_PBUS_SW_SCRATCH_0E)
//
#define FRTS_ERR_NONE               0x0000
#define FRTS_ERR_INVALID_CMD        0x0001
#define FRTS_ERR_WPR_ALREADY_SET    0x0002
#define FRTS_ERR_FB_SIZE_MISMATCH   0x0003
#define FRTS_ERR_SIGNATURE_FAIL     0x0004

#pragma pack(push, 1)

//
// Falcon Application Interface Header V1
// Located in DMEM at interface_offset
//
typedef struct {
    UINT8   Version;            // 0x01
    UINT8   HeaderSize;         // 4
    UINT8   EntrySize;          // 8
    UINT8   EntryCount;         // Number of entries
} FALCON_APPIF_HDR_V1;

//
// Falcon Application Interface Entry
//
typedef struct {
    UINT32  Id;                 // Application ID (DMEMMAPPER = 0x04)
    UINT32  DmemOffset;         // Offset in DMEM to app-specific data
} FALCON_APPIF_ENTRY;

//
// DMEM Mapper V3 (FalconAppifDmemmapperV3)
// Used to pass commands to FWSEC
//
typedef struct {
    UINT32  Signature;          // "DMAP" = 0x50414D44
    UINT32  Version;            // 0x00030000 or 0x00400003
    UINT32  Size;               // Structure size (64 bytes typical)
    UINT32  CmdInBufferOffset;  // Offset to command input buffer
    UINT32  CmdInBufferSize;    // Size of input buffer
    UINT32  CmdOutBufferOffset; // Offset to command output buffer
    UINT32  CmdOutBufferSize;   // Size of output buffer
    UINT32  InitCmd;            // Initial command
    UINT32  Features;           // Feature flags
    UINT32  CmdMask0;           // Command mask bits 0-31
    UINT32  CmdMask1;           // Command mask bits 32-63
    UINT8   Reserved[20];       // Padding to 64 bytes
} FALCON_APPIF_DMEMMAPPER_V3;

//
// FRTS Command Structure
// Written to CmdInBuffer to execute FWSEC-FRTS
//
typedef struct {
    UINT32  Cmd;                // FWSEC_CMD_FRTS (0x15)
    UINT32  FrtsRegionOffset;   // FRTS region offset in FB (from end)
    UINT32  FrtsRegionSize;     // FRTS region size (usually 1MB)
    UINT32  Reserved[5];        // Padding
} FWSEC_FRTS_CMD;

//
// FRTS Command Output
// Read from CmdOutBuffer after execution
//
typedef struct {
    UINT32  Status;             // 0 = success
    UINT32  ErrorCode;          // Error code if failed
    UINT32  Wpr2Lo;             // WPR2 low address (result)
    UINT32  Wpr2Hi;             // WPR2 high address (result)
    UINT32  Reserved[4];
} FWSEC_FRTS_OUTPUT;

//
// Heavy Secure Header V2 (for Booter firmware, not FWSEC directly)
//
typedef struct {
    UINT32  SigProdOffset;      // Offset to production signatures
    UINT32  SigProdSize;        // Size of production signatures
    UINT32  PatchLocOffset;     // Offset to patch location value
    UINT32  PatchSigOffset;     // Offset to patch signature index
    UINT32  MetaDataOffset;     // Offset to signature metadata
    UINT32  MetaDataSize;       // Size of signature metadata
    UINT32  NumSigOffset;       // Offset to number of signatures
    UINT32  HeaderOffset;       // Offset to application header
    UINT32  HeaderSize;         // Size of application header
} HS_HEADER_V2;

#pragma pack(pop)

//
// FWSEC Firmware Context
//
typedef struct {
    // Source VBIOS context
    VBIOS_CONTEXT       *Vbios;

    // FWSEC descriptor
    FALCON_UCODE_DESC_V3 Desc;

    // Firmware data
    UINT8               *ImemData;      // Instruction memory
    UINT32              ImemSize;
    UINT8               *DmemData;      // Data memory
    UINT32              DmemSize;

    // Signatures
    UINT8               *Signatures;    // RSA-3K signatures
    UINT32              SignatureCount;
    UINT32              SignatureSize;  // Size per signature (384)

    // DMEM Mapper (for patching FRTS command)
    FALCON_APPIF_DMEMMAPPER_V3 *DmemMapper;
    UINT32              DmemMapperOffset;

    // DMA buffer (combined IMEM + DMEM + signatures)
    UINT8               *DmaBuffer;
    UINT32              DmaBufferSize;
    EFI_PHYSICAL_ADDRESS DmaPhysAddr;

    // FRTS region info
    UINT64              FrtsBase;       // FRTS region base in FB
    UINT64              FrtsSize;       // FRTS region size
} FWSEC_CONTEXT;

//
// FRTS Region Layout
//
typedef struct {
    UINT64  FbSize;             // Total framebuffer size
    UINT64  FbUsable;           // Usable framebuffer size

    UINT64  VgaWorkspaceBase;   // VGA workspace base
    UINT64  VgaWorkspaceSize;   // VGA workspace size

    UINT64  FrtsBase;           // FRTS region base
    UINT64  FrtsSize;           // FRTS region size (1MB typical)

    UINT64  Wpr2Base;           // WPR2 region base (computed by FWSEC)
    UINT64  Wpr2Size;           // WPR2 region size
} FB_LAYOUT;

//
// Function Prototypes
//

/**
 * Initialize FWSEC context from VBIOS
 */
EFI_STATUS
FwsecInit (
    OUT FWSEC_CONTEXT   *Context,
    IN  VBIOS_CONTEXT   *Vbios
    );

/**
 * Extract FWSEC firmware from VBIOS
 */
EFI_STATUS
FwsecExtract (
    IN OUT FWSEC_CONTEXT *Context
    );

/**
 * Find and parse Application Interface
 */
EFI_STATUS
FwsecFindAppInterface (
    IN OUT FWSEC_CONTEXT *Context
    );

/**
 * Find DMEM Mapper in Application Interface
 */
EFI_STATUS
FwsecFindDmemMapper (
    IN OUT FWSEC_CONTEXT *Context
    );

/**
 * Patch FRTS command into DMEM
 */
EFI_STATUS
FwsecPatchFrtsCommand (
    IN OUT FWSEC_CONTEXT *Context,
    IN     FB_LAYOUT     *FbLayout
    );

/**
 * Select and patch signature based on fuse version
 */
EFI_STATUS
FwsecPatchSignature (
    IN OUT FWSEC_CONTEXT *Context,
    IN     UINT32        FuseVersion
    );

/**
 * Prepare DMA buffer for loading
 */
EFI_STATUS
FwsecPrepareDmaBuffer (
    IN OUT FWSEC_CONTEXT *Context
    );

/**
 * Compute FB layout for FRTS
 */
EFI_STATUS
FwsecComputeFbLayout (
    IN  UINT32          Bar0,
    OUT FB_LAYOUT       *Layout
    );

/**
 * Free FWSEC context resources
 */
VOID
FwsecFree (
    IN OUT FWSEC_CONTEXT *Context
    );

//
// Helper Functions
//

/**
 * Get signature index from fuse version
 */
UINT32
FwsecGetSignatureIndex (
    IN  UINT16  SignatureVersions,
    IN  UINT32  FuseVersion
    );

/**
 * Validate DMEM Mapper structure
 */
BOOLEAN
FwsecValidateDmemMapper (
    IN  FALCON_APPIF_DMEMMAPPER_V3 *Mapper
    );

#endif // NVDAAL_FWSEC_H
