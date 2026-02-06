/**
 * @file fwsec_impl.c
 * @brief FWSEC-FRTS implementation for NVIDIA Ada Lovelace GPUs
 *
 * Complete implementation based on NVIDIA open-gpu-kernel-modules:
 *   - kernel_gsp_frts_tu102.c (FRTS command structure and execution)
 *   - kernel_gsp_fwsec.c (VBIOS parsing and ucode extraction)
 *
 * Copyright (c) 2024-2025 Gabriel Maia / NVDAAL Project
 * SPDX-License-Identifier: MIT
 */

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>

#include "fwsec.h"
#include "falcon.h"
// Note: Using local renamed types (FWSEC_BIT_HDR, etc) to avoid conflicts

//==============================================================================
// Debug Logging - Use LogPrint from NvdaalFwsec.c to write to log file
//==============================================================================

// External logging function from NvdaalFwsec.c (variadic, writes to file)
extern VOID EFIAPI LogPrint (IN CONST CHAR16 *Format, ...);

// Use LogPrint directly (simpler and guaranteed to work)
#define LOG(fmt, ...)     LogPrint(L"NVDAAL: [FWSEC] " fmt L"\n", ##__VA_ARGS__)
#define LOG_DBG(fmt, ...) LogPrint(L"NVDAAL: [FWSEC-DBG] " fmt L"\n", ##__VA_ARGS__)
#define LOG_ERR(fmt, ...) LogPrint(L"NVDAAL: [FWSEC-ERR] " fmt L"\n", ##__VA_ARGS__)

//==============================================================================
// Constants
//==============================================================================

// BIT Header
#define FWSEC_BIT_HEADER_ID         0xB8FF
#define FWSEC_BIT_SIGNATURE         0x00544942  // "BIT\0"

// BIT Token IDs (local to avoid conflicts)
#define FWSEC_TOKEN_FALCON_DATA     0x70
#define FWSEC_TOKEN_BIOSDATA        0x42

// PMU Application IDs
#define PMU_APPID_FWSEC_PROD        0x85
#define PMU_APPID_FWSEC_DBG         0x45
#define PMU_APPID_FW_SEC_LIC        0x05

// Falcon Registers (local copies for self-contained module)
#define FWSEC_GSP_BASE              0x00110000
#define FWSEC_FALCON_CPUCTL         0x0100
#define FWSEC_FALCON_BOOTVEC        0x0104
#define FWSEC_FALCON_IMEMC(i)       (0x0180 + (i) * 16)
#define FWSEC_FALCON_IMEMD(i)       (0x0184 + (i) * 16)
#define FWSEC_FALCON_DMEMC(i)       (0x01C0 + (i) * 8)
#define FWSEC_FALCON_DMEMD(i)       (0x01C4 + (i) * 8)

#define FWSEC_CPUCTL_STARTCPU       (1 << 1)
#define FWSEC_CPUCTL_HALTED         (1 << 4)
#define FWSEC_MEM_AINCW             (1 << 24)

// WPR2 Registers
#define FWSEC_WPR2_ADDR_LO          0x001FA820
#define FWSEC_WPR2_ADDR_HI          0x001FA824

// Timeouts
#define FWSEC_HALT_TIMEOUT_US       5000000  // 5 seconds

//==============================================================================
// Local BIT/VBIOS Structures (renamed to avoid conflicts with vbios.h)
//==============================================================================

#pragma pack(push, 1)

typedef struct {
    UINT16  Id;             // 0xB8FF
    UINT32  Signature;      // "BIT\0"
    UINT16  BcdVersion;
    UINT8   HeaderSize;
    UINT8   TokenSize;
    UINT8   TokenEntries;
    UINT8   HeaderChksum;
} FWSEC_BIT_HDR;

typedef struct {
    UINT8   TokenId;
    UINT8   DataVersion;
    UINT16  DataSize;
    UINT32  DataPtr;
} FWSEC_BIT_TOK;

typedef struct {
    UINT32  FalconUcodeTablePtr;
} FWSEC_FALCON_DATA;

typedef struct {
    UINT8   Version;
    UINT8   HeaderSize;
    UINT8   EntrySize;
    UINT8   EntryCount;
    UINT8   DescVersion;
    UINT8   DescSize;
} FWSEC_PMU_HDR;

typedef struct {
    UINT8   ApplicationId;
    UINT8   TargetId;
    UINT32  DescPtr;
} FWSEC_PMU_ENTRY;

#pragma pack(pop)

//==============================================================================
// FwsecReadFuseVersion
// Based on kgspReadUcodeFuseVersion_HAL
//==============================================================================

UINT32
FwsecReadFuseVersion (
    IN  UINT32  Bar0,
    IN  UINT8   UcodeId
    )
{
    UINT32  FuseReg;
    UINT32  FuseVal;
    UINT32  Version;

    // UcodeId is 1-based, validate range
    if (UcodeId == 0 || UcodeId > 16) {
        LOG_DBG(L"Invalid UcodeId %d, using version 0", UcodeId);
        return 0;
    }

    // Calculate fuse register address
    // Each ucode has its own fuse register at 4-byte intervals
    FuseReg = NV_FUSE_OPT_FPF_GSP_UCODE1_VERSION + ((UINT32)(UcodeId - 1) * 4);

    FuseVal = GpuRead32(Bar0, FuseReg);
    LOG_DBG(L"Fuse register 0x%X = 0x%X", FuseReg, FuseVal);

    if (FuseVal == 0) {
        return 0;
    }

    // Find highest bit set (fuse version is encoded as bitmask)
    Version = 0;
    while (FuseVal >>= 1) {
        Version++;
    }

    LOG_DBG(L"UcodeId %d: fuse version = %d", UcodeId, Version + 1);
    return Version + 1;
}

//==============================================================================
// FindBitHeader - Find BIT header in VBIOS
//==============================================================================

static EFI_STATUS
FindBitHeader (
    IN  UINT8   *VbiosData,
    IN  UINTN   VbiosSize,
    OUT UINT32  *BitOffset
    )
{
    UINT32  Offset;
    UINT16  Id;
    UINT32  Sig;

    LOG(L"FindBitHeader: searching in %d bytes...", VbiosSize);

    // Search for BIT header pattern: 0xFFB8 followed by "BIT\0"
    for (Offset = 0; Offset < VbiosSize - 12; Offset++) {
        Id = *(UINT16 *)(VbiosData + Offset);
        if (Id == FWSEC_BIT_HEADER_ID) {
            Sig = *(UINT32 *)(VbiosData + Offset + 2);
            if (Sig == FWSEC_BIT_SIGNATURE) {
                // Verify checksum
                FWSEC_BIT_HDR *Hdr = (FWSEC_BIT_HDR *)(VbiosData + Offset);
                UINT8 Sum = 0;
                for (UINT32 i = 0; i < Hdr->HeaderSize; i++) {
                    Sum += VbiosData[Offset + i];
                }
                if ((Sum & 0xFF) == 0) {
                    *BitOffset = Offset;
                    LOG_DBG(L"Found BIT header at 0x%X", Offset);
                    return EFI_SUCCESS;
                }
            }
        }
    }

    return EFI_NOT_FOUND;
}

//==============================================================================
// FindFwsecDescriptor - Parse BIT to find FWSEC ucode descriptor
//==============================================================================

static EFI_STATUS
FindFwsecDescriptor (
    IN  UINT8               *VbiosData,
    IN  UINTN               VbiosSize,
    IN  UINT32              BitOffset,
    IN  UINT32              ExpansionRomOffset,
    OUT FALCON_UCODE_DESC_V3 *OutDesc,
    OUT UINT32              *OutDescOffset,
    OUT UINT32              *OutDescSize
    )
{
    FWSEC_BIT_HDR   *BitHdr;
    UINT32          TokenOffset;
    UINT32          i;

    LOG(L"FindFwsecDescriptor: BIT@0x%X ExpROM@0x%X", BitOffset, ExpansionRomOffset);

    BitHdr = (FWSEC_BIT_HDR *)(VbiosData + BitOffset);
    TokenOffset = BitOffset + BitHdr->HeaderSize;

    LOG(L"BIT has %d tokens, starting at 0x%X", BitHdr->TokenEntries, TokenOffset);

    // Iterate through BIT tokens
    for (i = 0; i < BitHdr->TokenEntries; i++) {
        FWSEC_BIT_TOK *Token = (FWSEC_BIT_TOK *)(VbiosData + TokenOffset);

        // Look for FALCON_DATA token (0x70)
        if (Token->TokenId == FWSEC_TOKEN_FALCON_DATA &&
            Token->DataVersion == 2 &&
            Token->DataSize >= 4) {
            LOG(L"Found FALCON_DATA token at 0x%X", TokenOffset);

            FWSEC_FALCON_DATA *FalconData;
            FWSEC_PMU_HDR *PmuHdr;
            UINT32 PmuTableOffset;
            UINT32 j;

            // DataPtr is relative to expansion ROM, not absolute
            FalconData = (FWSEC_FALCON_DATA *)(VbiosData + ExpansionRomOffset + Token->DataPtr);
            PmuTableOffset = ExpansionRomOffset + FalconData->FalconUcodeTablePtr;

            LOG_DBG(L"FALCON_DATA @ 0x%X: UcodeTablePtr=0x%X",
                    ExpansionRomOffset + Token->DataPtr, FalconData->FalconUcodeTablePtr);

            if (PmuTableOffset + sizeof(FWSEC_PMU_HDR) > VbiosSize) {
                LOG_ERR(L"PMU table offset out of bounds");
                goto next_token;
            }

            PmuHdr = (FWSEC_PMU_HDR *)(VbiosData + PmuTableOffset);

            LOG_DBG(L"PMU Header @ 0x%X: ver=%d hdr=%d entry=%d count=%d",
                    PmuTableOffset, PmuHdr->Version, PmuHdr->HeaderSize,
                    PmuHdr->EntrySize, PmuHdr->EntryCount);

            // Dump raw PMU table bytes for analysis
            LOG(L"PMU Table raw bytes at 0x%X:", PmuTableOffset);
            {
                UINT8 *PmuRaw = VbiosData + PmuTableOffset;
                LOG(L"  %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X",
                    PmuRaw[0], PmuRaw[1], PmuRaw[2], PmuRaw[3],
                    PmuRaw[4], PmuRaw[5], PmuRaw[6], PmuRaw[7],
                    PmuRaw[8], PmuRaw[9], PmuRaw[10], PmuRaw[11],
                    PmuRaw[12], PmuRaw[13], PmuRaw[14], PmuRaw[15]);
            }

            // Validate PMU header - version should be 1 for V1 table
            // NOTE: Version 231 (0xE7) indicates corrupted pointer per HuggingChat analysis
            if (PmuHdr->Version != 1 || PmuHdr->HeaderSize < 6 ||
                PmuHdr->EntrySize < 6 || PmuHdr->EntryCount == 0) {
                LOG_ERR(L"Invalid PMU table: ver=%d (0x%02X) hdr=%d entry=%d count=%d",
                        PmuHdr->Version, PmuHdr->Version, PmuHdr->HeaderSize,
                        PmuHdr->EntrySize, PmuHdr->EntryCount);
                LOG_ERR(L"Expected ver=1, got ver=%d - likely corrupted pointer!", PmuHdr->Version);

                // Try alternate interpretation: ASUS may use different offset
                // Check if this looks like clock/timing data instead
                LOG(L"Checking alternate PMU locations...");

                // Try offset 0x80 from BIT (instead of FALCON_DATA token)
                UINT32 AltOffset = ExpansionRomOffset + 0x80;
                if (AltOffset + 16 < VbiosSize) {
                    UINT8 *AltPmu = VbiosData + AltOffset;
                    LOG(L"Alt PMU @ 0x%X: %02X %02X %02X %02X %02X %02X %02X %02X",
                        AltOffset, AltPmu[0], AltPmu[1], AltPmu[2], AltPmu[3],
                        AltPmu[4], AltPmu[5], AltPmu[6], AltPmu[7]);
                }

                goto next_token;
            }

            LOG_DBG(L"PMU table at 0x%X: %d entries", PmuTableOffset, PmuHdr->EntryCount);

            // Search for FWSEC_PROD entry
            for (j = 0; j < PmuHdr->EntryCount; j++) {
                FWSEC_PMU_ENTRY *Entry = (FWSEC_PMU_ENTRY *)(
                    VbiosData + PmuTableOffset + PmuHdr->HeaderSize + j * PmuHdr->EntrySize);

                if (Entry->ApplicationId == PMU_APPID_FWSEC_PROD ||
                    Entry->ApplicationId == PMU_APPID_FW_SEC_LIC) {

                    UINT32 DescOffset = ExpansionRomOffset + Entry->DescPtr;
                    UINT32 VDescVal;
                    UINT8 DescVersion;
                    UINT32 DescSize;

                    if (DescOffset + 4 > VbiosSize) {
                        continue;
                    }

                    // Read VDesc to get version and size
                    VDescVal = *(UINT32 *)(VbiosData + DescOffset);

                    // Check version availability flag
                    if ((VDescVal & VDESC_FLAGS_VERSION_BIT) == 0) {
                        continue;
                    }

                    DescVersion = (UINT8)((VDescVal & VDESC_VERSION_MASK) >> VDESC_VERSION_SHIFT);
                    DescSize = (VDescVal & VDESC_SIZE_MASK) >> VDESC_SIZE_SHIFT;

                    LOG_DBG(L"Found FWSEC: app=0x%02X, desc v%d, size=%d at 0x%X",
                            Entry->ApplicationId, DescVersion, DescSize, DescOffset);

                    // We need V3 descriptor for Ada Lovelace
                    if (DescVersion == FALCON_UCODE_DESC_VERSION_V3 &&
                        DescSize >= FALCON_UCODE_DESC_V3_SIZE) {

                        if (DescOffset + FALCON_UCODE_DESC_V3_SIZE > VbiosSize) {
                            continue;
                        }

                        CopyMem(OutDesc, VbiosData + DescOffset, FALCON_UCODE_DESC_V3_SIZE);
                        *OutDescOffset = DescOffset;
                        *OutDescSize = DescSize;

                        LOG_DBG(L"FWSEC V3 descriptor:");
                        LOG_DBG(L"  StoredSize: 0x%X", OutDesc->StoredSize);
                        LOG_DBG(L"  PKCDataOffset: 0x%X", OutDesc->PKCDataOffset);
                        LOG_DBG(L"  InterfaceOffset: 0x%X", OutDesc->InterfaceOffset);
                        LOG_DBG(L"  IMEM: base=0x%X size=0x%X", OutDesc->IMEMPhysBase, OutDesc->IMEMLoadSize);
                        LOG_DBG(L"  DMEM: base=0x%X size=0x%X", OutDesc->DMEMPhysBase, OutDesc->DMEMLoadSize);
                        LOG_DBG(L"  UcodeId: %d, SigCount: %d, SigVersions: 0x%04X",
                                OutDesc->UcodeId, OutDesc->SignatureCount, OutDesc->SignatureVersions);

                        return EFI_SUCCESS;
                    }
                }
            }
        }

next_token:
        TokenOffset += BitHdr->TokenSize;
    }

    return EFI_NOT_FOUND;
}

//==============================================================================
// FwsecParseFromVbios
//==============================================================================

EFI_STATUS
FwsecParseFromVbios (
    OUT FWSEC_CONTEXT   *Context,
    IN  UINT32          Bar0,
    IN  UINT8           *VbiosData,
    IN  UINTN           VbiosSize
    )
{
    EFI_STATUS  Status;
    UINT32      BitOffset;
    UINT32      ExpansionRomOffset = 0;
    UINT32      ImageOffset;
    UINT32      SignaturesOffset;

    LOG(L"FwsecParseFromVbios: VBIOS=%p Size=%d Bar0=0x%X", VbiosData, VbiosSize, Bar0);

    ZeroMem(Context, sizeof(FWSEC_CONTEXT));
    Context->Bar0 = Bar0;
    Context->VbiosData = VbiosData;
    Context->VbiosSize = VbiosSize;

    LOG(L"Searching for expansion ROM (0xAA55)...");

    // Find expansion ROM offset (first 0x55AA signature)
    for (UINT32 Off = 0; Off < VbiosSize - 2; Off += 0x100) {
        if (*(UINT16 *)(VbiosData + Off) == 0xAA55) {
            ExpansionRomOffset = Off;
            LOG_DBG(L"Expansion ROM at 0x%X", ExpansionRomOffset);
            break;
        }
    }
    Context->ExpansionRomOffset = ExpansionRomOffset;

    // Find BIT header
    Status = FindBitHeader(VbiosData, VbiosSize, &BitOffset);
    if (EFI_ERROR(Status)) {
        LOG_ERR(L"BIT header not found in VBIOS");
        return Status;
    }

    // Find FWSEC descriptor
    Status = FindFwsecDescriptor(VbiosData, VbiosSize, BitOffset, ExpansionRomOffset,
                                  &Context->UcodeDesc, &Context->UcodeDescOffset,
                                  &Context->UcodeDescSize);
    if (EFI_ERROR(Status)) {
        LOG_ERR(L"FWSEC descriptor not found");
        return Status;
    }

    // Calculate offsets for IMEM, DMEM, and signatures
    // Layout: [Descriptor][Signatures][IMEM][DMEM]
    SignaturesOffset = Context->UcodeDescOffset + FALCON_UCODE_DESC_V3_SIZE;
    Context->SignaturesTotalSize = Context->UcodeDescSize - FALCON_UCODE_DESC_V3_SIZE;

    ImageOffset = Context->UcodeDescOffset + Context->UcodeDescSize;
    Context->ImemSize = Context->UcodeDesc.IMEMLoadSize;
    Context->DmemSize = Context->UcodeDesc.DMEMLoadSize;

    LOG_DBG(L"Signatures at 0x%X, total size: %d", SignaturesOffset, Context->SignaturesTotalSize);
    LOG_DBG(L"Image at 0x%X, IMEM: %d, DMEM: %d", ImageOffset, Context->ImemSize, Context->DmemSize);

    // Validate sizes
    if (ImageOffset + Context->ImemSize + Context->DmemSize > VbiosSize) {
        LOG_ERR(L"FWSEC image extends beyond VBIOS");
        return EFI_INVALID_PARAMETER;
    }

    // Allocate and copy signatures
    if (Context->SignaturesTotalSize > 0) {
        Context->Signatures = AllocatePool(Context->SignaturesTotalSize);
        if (Context->Signatures == NULL) {
            return EFI_OUT_OF_RESOURCES;
        }
        CopyMem(Context->Signatures, VbiosData + SignaturesOffset, Context->SignaturesTotalSize);
    }

    // Allocate and copy IMEM
    if (Context->ImemSize > 0) {
        Context->ImemData = AllocatePool(Context->ImemSize);
        if (Context->ImemData == NULL) {
            return EFI_OUT_OF_RESOURCES;
        }
        CopyMem(Context->ImemData, VbiosData + ImageOffset, Context->ImemSize);
    }

    // Allocate and copy DMEM (we'll patch this)
    if (Context->DmemSize > 0) {
        Context->DmemData = AllocatePool(Context->DmemSize);
        if (Context->DmemData == NULL) {
            return EFI_OUT_OF_RESOURCES;
        }
        CopyMem(Context->DmemData, VbiosData + ImageOffset + Context->ImemSize, Context->DmemSize);
    }

    // Read fuse version for signature selection
    Context->FuseVersion = (UINT8)FwsecReadFuseVersion(Bar0, Context->UcodeDesc.UcodeId);

    return EFI_SUCCESS;
}

//==============================================================================
// FwsecSelectSignature
// Based on NVIDIA signature selection algorithm (lines 357-378)
//==============================================================================

EFI_STATUS
FwsecSelectSignature (
    IN OUT FWSEC_CONTEXT *Context
    )
{
    UINT32  UcodeVersionVal;
    UINT16  HsSigVersions;
    UINT32  SigOffset;

    if (Context->Signatures == NULL || Context->SignaturesTotalSize == 0) {
        LOG_ERR(L"No signatures available");
        return EFI_NOT_FOUND;
    }

    // Convert fuse version to bitmask (1 << version)
    UcodeVersionVal = 1 << Context->FuseVersion;
    HsSigVersions = Context->UcodeDesc.SignatureVersions;

    LOG_DBG(L"Selecting signature: fuse=%d, ucodeVer=0x%X, sigVersions=0x%04X",
            Context->FuseVersion, UcodeVersionVal, HsSigVersions);

    // Check if requested version is available
    if ((UcodeVersionVal & HsSigVersions) == 0) {
        LOG_ERR(L"Required signature version not available");
        return EFI_NOT_FOUND;
    }

    // Calculate offset to correct signature
    // Walk through the bitmask, counting signatures until we reach ours
    SigOffset = 0;
    while ((UcodeVersionVal & HsSigVersions & 1) == 0) {
        SigOffset += (HsSigVersions & 1) * RSA3K_SIGNATURE_SIZE;
        HsSigVersions >>= 1;
        UcodeVersionVal >>= 1;
    }

    if (SigOffset >= Context->SignaturesTotalSize) {
        LOG_ERR(L"Signature offset 0x%X exceeds available 0x%X",
                SigOffset, Context->SignaturesTotalSize);
        return EFI_INVALID_PARAMETER;
    }

    Context->SelectedSigOffset = SigOffset;
    LOG_DBG(L"Selected signature at offset 0x%X", SigOffset);

    return EFI_SUCCESS;
}

//==============================================================================
// FwsecPatchSignature
// Patch RSA signature into DMEM at PKCDataOffset
//==============================================================================

EFI_STATUS
FwsecPatchSignature (
    IN OUT FWSEC_CONTEXT *Context
    )
{
    UINT32  PkcOffset;

    if (Context->DmemData == NULL) {
        return EFI_NOT_READY;
    }

    PkcOffset = Context->UcodeDesc.PKCDataOffset;

    // Validate offset
    if (PkcOffset + RSA3K_SIGNATURE_SIZE > Context->DmemSize) {
        LOG_ERR(L"PKCDataOffset 0x%X + sig size exceeds DMEM", PkcOffset);
        return EFI_INVALID_PARAMETER;
    }

    // Copy selected signature to DMEM
    CopyMem(Context->DmemData + PkcOffset,
            Context->Signatures + Context->SelectedSigOffset,
            RSA3K_SIGNATURE_SIZE);

    LOG(L"Patched RSA-3K signature at DMEM offset 0x%X", PkcOffset);
    return EFI_SUCCESS;
}

//==============================================================================
// FwsecPatchFrtsCmd
// Patch FRTS command into DMEMMAPPER
// Based on s_vbiosPatchInterfaceData
//==============================================================================

EFI_STATUS
FwsecPatchFrtsCmd (
    IN OUT FWSEC_CONTEXT *Context,
    IN     UINT64        FrtsOffset
    )
{
    FALCON_APPIF_HEADER *AppifHdr;
    FALCON_APPIF_ENTRY  *Entries;
    FALCON_DMEMMAPPER   *DmemMapper;
    FWSEC_FRTS_CMD      FrtsCmd;
    UINT32              InterfaceOffset;
    UINT32              i;

    if (Context->DmemData == NULL) {
        return EFI_NOT_READY;
    }

    Context->FrtsOffset = FrtsOffset;
    InterfaceOffset = Context->UcodeDesc.InterfaceOffset;

    // Validate interface offset
    if (InterfaceOffset + sizeof(FALCON_APPIF_HEADER) > Context->DmemSize) {
        LOG_ERR(L"Interface offset 0x%X out of bounds", InterfaceOffset);
        return EFI_INVALID_PARAMETER;
    }

    AppifHdr = (FALCON_APPIF_HEADER *)(Context->DmemData + InterfaceOffset);

    if (AppifHdr->EntryCount < 2) {
        LOG_ERR(L"Too few interface entries: %d", AppifHdr->EntryCount);
        return EFI_INVALID_PARAMETER;
    }

    LOG_DBG(L"Appif header: ver=%d, hdr=%d, entry=%d, count=%d",
            AppifHdr->Version, AppifHdr->HeaderSize, AppifHdr->EntrySize, AppifHdr->EntryCount);

    // Find DMEMMAPPER entry
    Entries = (FALCON_APPIF_ENTRY *)(Context->DmemData + InterfaceOffset + sizeof(FALCON_APPIF_HEADER));
    DmemMapper = NULL;

    for (i = 0; i < AppifHdr->EntryCount; i++) {
        if (Entries[i].Id == APPIF_ENTRY_ID_DMEMMAPPER) {
            UINT32 MapperOffset = Entries[i].DmemOffset;

            if (MapperOffset + sizeof(FALCON_DMEMMAPPER) > Context->DmemSize) {
                LOG_ERR(L"DMEMMAPPER offset out of bounds");
                return EFI_INVALID_PARAMETER;
            }

            DmemMapper = (FALCON_DMEMMAPPER *)(Context->DmemData + MapperOffset);

            if (DmemMapper->Signature != DMEMMAPPER_SIGNATURE) {
                LOG_ERR(L"Invalid DMEMMAPPER signature: 0x%08X", DmemMapper->Signature);
                return EFI_INVALID_PARAMETER;
            }

            LOG_DBG(L"Found DMEMMAPPER at 0x%X", MapperOffset);
            break;
        }
    }

    if (DmemMapper == NULL) {
        LOG_ERR(L"DMEMMAPPER not found");
        return EFI_NOT_FOUND;
    }

    // Patch init_cmd to FRTS
    LOG_DBG(L"Patching InitCmd: 0x%X -> 0x%X", DmemMapper->InitCmd, FWSEC_CMD_FRTS);
    DmemMapper->InitCmd = FWSEC_CMD_FRTS;

    // Build FRTS command structure (matching NVIDIA exactly)
    ZeroMem(&FrtsCmd, sizeof(FrtsCmd));

    // readVbiosDesc
    FrtsCmd.ReadVbiosDesc.Version = 1;
    FrtsCmd.ReadVbiosDesc.Size = sizeof(FWSEC_READ_VBIOS_DESC);
    FrtsCmd.ReadVbiosDesc.GfwImageOffset = 0;
    FrtsCmd.ReadVbiosDesc.GfwImageSize = 0;
    FrtsCmd.ReadVbiosDesc.Flags = FWSEC_READ_VBIOS_STRUCT_FLAGS;  // = 2

    // frtsRegionDesc
    FrtsCmd.FrtsRegionDesc.Version = 1;
    FrtsCmd.FrtsRegionDesc.Size = sizeof(FWSEC_FRTS_REGION_DESC);
    FrtsCmd.FrtsRegionDesc.FrtsOffset4K = (UINT32)(FrtsOffset >> 12);
    FrtsCmd.FrtsRegionDesc.FrtsSize4K = FRTS_SIZE_1MB_IN_4K;  // 0x100 = 1MB
    FrtsCmd.FrtsRegionDesc.MediaType = FRTS_REGION_MEDIA_FB;  // = 2

    // Validate cmd buffer
    if (DmemMapper->CmdInBufferSize < sizeof(FWSEC_FRTS_CMD)) {
        LOG_ERR(L"Cmd buffer too small: %d < %d",
                DmemMapper->CmdInBufferSize, sizeof(FWSEC_FRTS_CMD));
        return EFI_BUFFER_TOO_SMALL;
    }

    // Copy command to cmd_in_buffer
    UINT32 CmdOffset = (UINT32)((UINT8 *)DmemMapper - Context->DmemData) + DmemMapper->CmdInBufferOffset;
    if (CmdOffset + sizeof(FWSEC_FRTS_CMD) > Context->DmemSize) {
        // CmdInBufferOffset might be relative to DMEM start, not mapper
        CmdOffset = DmemMapper->CmdInBufferOffset;
    }

    CopyMem(Context->DmemData + CmdOffset, &FrtsCmd, sizeof(FrtsCmd));

    LOG(L"Patched FRTS command at 0x%X: offset4K=0x%X, size4K=0x%X",
        CmdOffset, FrtsCmd.FrtsRegionDesc.FrtsOffset4K, FrtsCmd.FrtsRegionDesc.FrtsSize4K);

    return EFI_SUCCESS;
}

//==============================================================================
// FwsecLoadUcode - Load IMEM/DMEM into Falcon
//==============================================================================

EFI_STATUS
FwsecLoadUcode (
    IN  FWSEC_CONTEXT *Context
    )
{
    UINT32  Bar0 = Context->Bar0;
    UINT32  FalconBase = FWSEC_GSP_BASE;
    UINT32  i;

    LOG(L"Loading FWSEC ucode: IMEM=%d bytes, DMEM=%d bytes",
        Context->ImemSize, Context->DmemSize);

    // Reset Falcon
    GpuWrite32(Bar0, FalconBase + FWSEC_FALCON_CPUCTL, 0);

    // Wait for halt
    for (i = 0; i < 1000; i++) {
        UINT32 Cpuctl = GpuRead32(Bar0, FalconBase + FWSEC_FALCON_CPUCTL);
        if (Cpuctl & FWSEC_CPUCTL_HALTED) {
            break;
        }
        // Small delay
        for (volatile int j = 0; j < 1000; j++);
    }

    // Load IMEM (in 256-byte blocks)
    LOG_DBG(L"Loading IMEM...");
    for (i = 0; i < Context->ImemSize; i += 4) {
        if ((i % 256) == 0) {
            // Set IMEMC for this block
            UINT32 Block = i / 256;
            GpuWrite32(Bar0, FalconBase + FWSEC_FALCON_IMEMC(0), (Block << 8) | FWSEC_MEM_AINCW);
        }

        UINT32 Word = *(UINT32 *)(Context->ImemData + i);
        GpuWrite32(Bar0, FalconBase + FWSEC_FALCON_IMEMD(0), Word);
    }

    // Load DMEM (in 256-byte blocks)
    LOG_DBG(L"Loading DMEM...");
    for (i = 0; i < Context->DmemSize; i += 4) {
        if ((i % 256) == 0) {
            // Set DMEMC for this block
            UINT32 Block = i / 256;
            GpuWrite32(Bar0, FalconBase + FWSEC_FALCON_DMEMC(0), (Block << 8) | FWSEC_MEM_AINCW);
        }

        UINT32 Word = *(UINT32 *)(Context->DmemData + i);
        GpuWrite32(Bar0, FalconBase + FWSEC_FALCON_DMEMD(0), Word);
    }

    LOG(L"Ucode loaded successfully");
    return EFI_SUCCESS;
}

//==============================================================================
// FwsecExecute - Start Falcon and wait for completion
//==============================================================================

EFI_STATUS
FwsecExecute (
    IN  FWSEC_CONTEXT *Context
    )
{
    UINT32  Bar0 = Context->Bar0;
    UINT32  FalconBase = FWSEC_GSP_BASE;
    UINT32  BootVec;
    UINTN   Timeout = FWSEC_HALT_TIMEOUT_US;

    // Set boot vector
    BootVec = Context->UcodeDesc.IMEMVirtBase;
    GpuWrite32(Bar0, FalconBase + FWSEC_FALCON_BOOTVEC, BootVec);
    LOG(L"Starting Falcon at boot vector 0x%X", BootVec);

    // Start CPU
    GpuWrite32(Bar0, FalconBase + FWSEC_FALCON_CPUCTL, FWSEC_CPUCTL_STARTCPU);

    // Wait for completion (halt)
    while (Timeout > 0) {
        UINT32 Cpuctl = GpuRead32(Bar0, FalconBase + FWSEC_FALCON_CPUCTL);

        if (Cpuctl & FWSEC_CPUCTL_HALTED) {
            // Check error scratch
            UINT32 Scratch0E = GpuRead32(Bar0, NV_PBUS_VBIOS_SCRATCH_0E);
            UINT16 FrtsErr = (UINT16)((Scratch0E >> 16) & 0xFFFF);

            if (FrtsErr != 0) {
                LOG_ERR(L"FWSEC execution failed: FRTS error 0x%04X", FrtsErr);
                return EFI_DEVICE_ERROR;
            }

            LOG(L"Falcon halted successfully");
            return EFI_SUCCESS;
        }

        // Wait 1ms
        for (volatile int j = 0; j < 100000; j++);
        Timeout -= 1000;
    }

    LOG_ERR(L"Falcon execution timeout");
    return EFI_TIMEOUT;
}

//==============================================================================
// FwsecVerifyWpr2
//==============================================================================

EFI_STATUS
FwsecVerifyWpr2 (
    IN  FWSEC_CONTEXT *Context
    )
{
    UINT32  Bar0 = Context->Bar0;
    UINT32  Wpr2Lo, Wpr2Hi;
    UINT32  ExpectedLo;

    Wpr2Lo = GpuRead32(Bar0, FWSEC_WPR2_ADDR_LO);
    Wpr2Hi = GpuRead32(Bar0, FWSEC_WPR2_ADDR_HI);

    LOG(L"WPR2: LO=0x%08X HI=0x%08X", Wpr2Lo, Wpr2Hi);

    // Check if WPR2 is configured (HI != 0)
    if ((Wpr2Hi & 0xFFFFFFF0) == 0) {
        LOG_ERR(L"WPR2 not configured (HI is zero)");
        return EFI_DEVICE_ERROR;
    }

    // Verify WPR2 LO matches expected FRTS offset
    ExpectedLo = (UINT32)(Context->FrtsOffset >> WPR2_ADDR_ALIGNMENT);
    if ((Wpr2Lo & 0xFFFFFFF0) != (ExpectedLo & 0xFFFFFFF0)) {
        LOG_ERR(L"WPR2 LO mismatch: got 0x%X, expected 0x%X", Wpr2Lo, ExpectedLo);
        return EFI_DEVICE_ERROR;
    }

    LOG(L"WPR2 configured correctly at 0x%llX",
        ((UINT64)(Wpr2Lo & 0xFFFFFFF0)) << 8);

    return EFI_SUCCESS;
}

//==============================================================================
// FwsecExecuteFrts - Main entry point
//==============================================================================

EFI_STATUS
FwsecExecuteFrts (
    IN  UINT32  Bar0,
    IN  UINT8   *VbiosData,
    IN  UINTN   VbiosSize,
    IN  UINT64  FrtsOffset
    )
{
    EFI_STATUS      Status;
    FWSEC_CONTEXT   Context;

    LOG(L"=== FWSEC-FRTS Execution Starting ===");
    LOG(L"VBIOS: %d bytes, FRTS offset: 0x%llX", VbiosSize, FrtsOffset);

    // Step 1: Parse VBIOS and extract FWSEC
    LOG(L"Step 1: Parsing VBIOS...");
    Status = FwsecParseFromVbios(&Context, Bar0, VbiosData, VbiosSize);
    if (EFI_ERROR(Status)) {
        LOG_ERR(L"Failed to parse VBIOS: %r", Status);
        return Status;
    }

    // Step 2: Select signature based on fuse version
    LOG(L"Step 2: Selecting signature (fuse version %d)...", Context.FuseVersion);
    Status = FwsecSelectSignature(&Context);
    if (EFI_ERROR(Status)) {
        LOG_ERR(L"Failed to select signature: %r", Status);
        goto cleanup;
    }

    // Step 3: Patch signature into DMEM
    LOG(L"Step 3: Patching signature...");
    Status = FwsecPatchSignature(&Context);
    if (EFI_ERROR(Status)) {
        LOG_ERR(L"Failed to patch signature: %r", Status);
        goto cleanup;
    }

    // Step 4: Patch FRTS command into DMEMMAPPER
    LOG(L"Step 4: Patching FRTS command...");
    Status = FwsecPatchFrtsCmd(&Context, FrtsOffset);
    if (EFI_ERROR(Status)) {
        LOG_ERR(L"Failed to patch FRTS command: %r", Status);
        goto cleanup;
    }

    // Step 5: Load ucode into Falcon
    LOG(L"Step 5: Loading ucode...");
    Status = FwsecLoadUcode(&Context);
    if (EFI_ERROR(Status)) {
        LOG_ERR(L"Failed to load ucode: %r", Status);
        goto cleanup;
    }

    // Step 6: Execute Falcon
    LOG(L"Step 6: Executing FWSEC...");
    Status = FwsecExecute(&Context);
    if (EFI_ERROR(Status)) {
        LOG_ERR(L"FWSEC execution failed: %r", Status);
        goto cleanup;
    }

    // Step 7: Verify WPR2
    LOG(L"Step 7: Verifying WPR2...");
    Status = FwsecVerifyWpr2(&Context);
    if (EFI_ERROR(Status)) {
        LOG_ERR(L"WPR2 verification failed: %r", Status);
        goto cleanup;
    }

    LOG(L"=== FWSEC-FRTS Success! WPR2 Configured ===");

cleanup:
    FwsecFreeContext(&Context);
    return Status;
}

//==============================================================================
// FwsecParseFromFile - Parse FWSEC from extracted file (FWSC format)
//==============================================================================

EFI_STATUS
FwsecParseFromFile (
    OUT FWSEC_CONTEXT   *Context,
    IN  UINT32          Bar0,
    IN  UINT8           *FileData,
    IN  UINTN           FileSize
    )
{
    FWSC_FILE_HEADER    *FileHdr;
    UINT8               *Payload;
    UINT32              SigTotalSize;
    UINT32              ImageOffset;

    LOG(L"FwsecParseFromFile: FileData=%p Size=%u Bar0=0x%X",
        FileData, FileSize, Bar0);

    if (FileSize < FWSC_HEADER_SIZE + FALCON_UCODE_DESC_V3_SIZE) {
        LOG_ERR(L"File too small: %u bytes", FileSize);
        return EFI_INVALID_PARAMETER;
    }

    FileHdr = (FWSC_FILE_HEADER *)FileData;

    // Validate magic
    if (FileHdr->Magic != FWSC_MAGIC) {
        LOG_ERR(L"Invalid FWSC magic: 0x%08X (expected 0x%08X)",
                FileHdr->Magic, FWSC_MAGIC);
        return EFI_INVALID_PARAMETER;
    }

    if (FileHdr->Version != FWSC_VERSION) {
        LOG_ERR(L"Unsupported FWSC version: %u", FileHdr->Version);
        return EFI_INVALID_PARAMETER;
    }

    LOG(L"FWSC header: descSize=%u totalDescSize=%u storedSize=%u payload=%u",
        FileHdr->DescSize, FileHdr->TotalDescSize,
        FileHdr->StoredSize, FileHdr->TotalPayloadSize);

    // Validate sizes
    if (FWSC_HEADER_SIZE + FileHdr->TotalPayloadSize > FileSize) {
        LOG_ERR(L"File truncated: need %u, have %u",
                FWSC_HEADER_SIZE + FileHdr->TotalPayloadSize, FileSize);
        return EFI_INVALID_PARAMETER;
    }

    // Payload starts after the FWSC header
    Payload = FileData + FWSC_HEADER_SIZE;

    ZeroMem(Context, sizeof(FWSEC_CONTEXT));
    Context->Bar0 = Bar0;

    // Copy V3 descriptor
    if (FileHdr->DescSize != FALCON_UCODE_DESC_V3_SIZE) {
        LOG_ERR(L"Unexpected desc size: %u (expected %u)",
                FileHdr->DescSize, FALCON_UCODE_DESC_V3_SIZE);
        return EFI_INVALID_PARAMETER;
    }

    CopyMem(&Context->UcodeDesc, Payload, FALCON_UCODE_DESC_V3_SIZE);

    LOG(L"V3 Descriptor loaded:");
    LOG(L"  StoredSize: 0x%X (%u)", Context->UcodeDesc.StoredSize, Context->UcodeDesc.StoredSize);
    LOG(L"  PKCDataOffset: 0x%X", Context->UcodeDesc.PKCDataOffset);
    LOG(L"  InterfaceOffset: 0x%X", Context->UcodeDesc.InterfaceOffset);
    LOG(L"  IMEM: phys=0x%X load=0x%X virt=0x%X",
        Context->UcodeDesc.IMEMPhysBase, Context->UcodeDesc.IMEMLoadSize,
        Context->UcodeDesc.IMEMVirtBase);
    LOG(L"  DMEM: phys=0x%X load=0x%X",
        Context->UcodeDesc.DMEMPhysBase, Context->UcodeDesc.DMEMLoadSize);
    LOG(L"  EngineIdMask: 0x%04X, UcodeId: %u",
        Context->UcodeDesc.EngineIdMask, Context->UcodeDesc.UcodeId);
    LOG(L"  SigCount: %u, SigVersions: 0x%04X",
        Context->UcodeDesc.SignatureCount, Context->UcodeDesc.SignatureVersions);

    // Signatures are between descriptor and image data
    SigTotalSize = FileHdr->TotalDescSize - FALCON_UCODE_DESC_V3_SIZE;
    Context->SignaturesTotalSize = SigTotalSize;

    if (SigTotalSize > 0) {
        Context->Signatures = AllocatePool(SigTotalSize);
        if (Context->Signatures == NULL) {
            return EFI_OUT_OF_RESOURCES;
        }
        CopyMem(Context->Signatures, Payload + FALCON_UCODE_DESC_V3_SIZE, SigTotalSize);
        LOG(L"Signatures: %u bytes (%u signatures of %u bytes each)",
            SigTotalSize, SigTotalSize / RSA3K_SIGNATURE_SIZE, RSA3K_SIGNATURE_SIZE);
    }

    // IMEM and DMEM follow after TotalDescSize
    ImageOffset = FileHdr->TotalDescSize;
    Context->ImemSize = Context->UcodeDesc.IMEMLoadSize;
    Context->DmemSize = Context->UcodeDesc.DMEMLoadSize;

    LOG(L"Image at payload offset 0x%X: IMEM=%u DMEM=%u",
        ImageOffset, Context->ImemSize, Context->DmemSize);

    // Validate
    if (ImageOffset + Context->ImemSize + Context->DmemSize > FileHdr->TotalPayloadSize) {
        LOG_ERR(L"Image extends beyond payload");
        return EFI_INVALID_PARAMETER;
    }

    // Allocate and copy IMEM
    if (Context->ImemSize > 0) {
        Context->ImemData = AllocatePool(Context->ImemSize);
        if (Context->ImemData == NULL) {
            return EFI_OUT_OF_RESOURCES;
        }
        CopyMem(Context->ImemData, Payload + ImageOffset, Context->ImemSize);
        LOG(L"IMEM loaded: %u bytes", Context->ImemSize);
    }

    // Allocate and copy DMEM (working copy for patching)
    if (Context->DmemSize > 0) {
        Context->DmemData = AllocatePool(Context->DmemSize);
        if (Context->DmemData == NULL) {
            return EFI_OUT_OF_RESOURCES;
        }
        CopyMem(Context->DmemData, Payload + ImageOffset + Context->ImemSize, Context->DmemSize);
        LOG(L"DMEM loaded: %u bytes", Context->DmemSize);

        // Dump APPIF header at InterfaceOffset for debugging
        if (Context->UcodeDesc.InterfaceOffset + 4 <= Context->DmemSize) {
            UINT8 *Appif = Context->DmemData + Context->UcodeDesc.InterfaceOffset;
            LOG_DBG(L"APPIF @ DMEM[0x%X]: ver=%u hdr=%u entry=%u count=%u",
                    Context->UcodeDesc.InterfaceOffset,
                    Appif[0], Appif[1], Appif[2], Appif[3]);
        }
    }

    // Read fuse version for signature selection
    Context->FuseVersion = (UINT8)FwsecReadFuseVersion(Bar0, Context->UcodeDesc.UcodeId);

    LOG(L"FWSEC context ready: fuseVersion=%u", Context->FuseVersion);
    return EFI_SUCCESS;
}

//==============================================================================
// FwsecExecuteFrtsFromFile - Execute FWSEC-FRTS from extracted file
//==============================================================================

EFI_STATUS
FwsecExecuteFrtsFromFile (
    IN  UINT32  Bar0,
    IN  UINT8   *FwsecFileData,
    IN  UINTN   FwsecFileSize,
    IN  UINT64  FrtsOffset
    )
{
    EFI_STATUS      Status;
    FWSEC_CONTEXT   Context;

    LOG(L"=== FWSEC-FRTS Execution from File ===");
    LOG(L"File: %u bytes, FRTS offset: 0x%llX", FwsecFileSize, FrtsOffset);

    // Step 1: Parse FWSEC from file
    LOG(L"Step 1: Parsing FWSEC from file...");
    Status = FwsecParseFromFile(&Context, Bar0, FwsecFileData, FwsecFileSize);
    if (EFI_ERROR(Status)) {
        LOG_ERR(L"Failed to parse FWSEC file: %r", Status);
        return Status;
    }

    // Step 2: Select signature based on fuse version
    LOG(L"Step 2: Selecting signature (fuse version %d)...", Context.FuseVersion);
    Status = FwsecSelectSignature(&Context);
    if (EFI_ERROR(Status)) {
        LOG_ERR(L"Failed to select signature: %r", Status);
        goto cleanup;
    }

    // Step 3: Patch signature into DMEM
    LOG(L"Step 3: Patching signature...");
    Status = FwsecPatchSignature(&Context);
    if (EFI_ERROR(Status)) {
        LOG_ERR(L"Failed to patch signature: %r", Status);
        goto cleanup;
    }

    // Step 4: Patch FRTS command into DMEMMAPPER
    LOG(L"Step 4: Patching FRTS command...");
    Status = FwsecPatchFrtsCmd(&Context, FrtsOffset);
    if (EFI_ERROR(Status)) {
        LOG_ERR(L"Failed to patch FRTS command: %r", Status);
        goto cleanup;
    }

    // Step 5: Load ucode into Falcon
    LOG(L"Step 5: Loading ucode...");
    Status = FwsecLoadUcode(&Context);
    if (EFI_ERROR(Status)) {
        LOG_ERR(L"Failed to load ucode: %r", Status);
        goto cleanup;
    }

    // Step 6: Execute Falcon
    LOG(L"Step 6: Executing FWSEC...");
    Status = FwsecExecute(&Context);
    if (EFI_ERROR(Status)) {
        LOG_ERR(L"FWSEC execution failed: %r", Status);
        goto cleanup;
    }

    // Step 7: Verify WPR2
    LOG(L"Step 7: Verifying WPR2...");
    Status = FwsecVerifyWpr2(&Context);
    if (EFI_ERROR(Status)) {
        LOG_ERR(L"WPR2 verification failed: %r", Status);
        goto cleanup;
    }

    LOG(L"=== FWSEC-FRTS from File: Success! WPR2 Configured ===");

cleanup:
    FwsecFreeContext(&Context);
    return Status;
}

//==============================================================================
// FwsecFreeContext
//==============================================================================

VOID
FwsecFreeContext (
    IN OUT FWSEC_CONTEXT *Context
    )
{
    if (Context->ImemData != NULL) {
        FreePool(Context->ImemData);
        Context->ImemData = NULL;
    }
    if (Context->DmemData != NULL) {
        FreePool(Context->DmemData);
        Context->DmemData = NULL;
    }
    if (Context->Signatures != NULL) {
        FreePool(Context->Signatures);
        Context->Signatures = NULL;
    }
}
