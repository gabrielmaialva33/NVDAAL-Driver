/**
 * @file fwsec_impl.c
 * @brief FWSEC implementation for NVDAAL EFI driver
 *
 * This implements the FWSEC extraction and execution sequence
 * similar to Linux nova-core driver.
 */

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/IoLib.h>

#include "vbios.h"
#include "fwsec.h"
#include "falcon.h"

//
// Debug logging macro
//
#define FWSEC_LOG(fmt, ...) \
    Print(L"NVDAAL-FWSEC: " fmt L"\n", ##__VA_ARGS__)

#define FWSEC_DEBUG(fmt, ...) \
    Print(L"NVDAAL-FWSEC: [DBG] " fmt L"\n", ##__VA_ARGS__)

//
// Constants
//
#define FRTS_SIZE               SIZE_1MB
#define FRTS_ALIGN              SIZE_1MB
#define VGA_WORKSPACE_SIZE      SIZE_256KB

#define DMA_TIMEOUT_US          1000000     // 1 second
#define GFW_BOOT_TIMEOUT_US     2000000     // 2 seconds
#define FALCON_HALT_TIMEOUT_US  5000000     // 5 seconds

//==============================================================================
// VBIOS Parsing Implementation
//==============================================================================

EFI_STATUS
VbiosInit (
    OUT VBIOS_CONTEXT   *Context,
    IN  UINT8           *RomData,
    IN  UINTN           RomSize
    )
{
    if (Context == NULL || RomData == NULL || RomSize == 0) {
        return EFI_INVALID_PARAMETER;
    }

    ZeroMem(Context, sizeof(VBIOS_CONTEXT));
    Context->RomData = RomData;
    Context->RomSize = RomSize;

    return EFI_SUCCESS;
}

EFI_STATUS
VbiosFindRomBase (
    IN  VBIOS_CONTEXT   *Context,
    OUT UINT32          *RomBase
    )
{
    UINT32  Offset;
    UINT16  Signature;
    UINT16  PcirOffset;

    // Search for ROM signature (0x55AA)
    for (Offset = 0; Offset < Context->RomSize - 0x20; Offset += 0x100) {
        Signature = *(UINT16 *)(Context->RomData + Offset);
        if (Signature == VBIOS_ROM_SIGNATURE) {
            // Verify PCIR structure
            PcirOffset = *(UINT16 *)(Context->RomData + Offset + 0x18);
            if (PcirOffset > 0 && Offset + PcirOffset + 24 < Context->RomSize) {
                UINT32 PcirSig = *(UINT32 *)(Context->RomData + Offset + PcirOffset);
                if (PcirSig == VBIOS_PCIR_SIGNATURE) {
                    UINT8 CodeType = *(Context->RomData + Offset + PcirOffset + 0x14);
                    if (CodeType == PCIR_CODE_TYPE_X86) {
                        Context->RomBase = Offset;
                        *RomBase = Offset;
                        FWSEC_DEBUG(L"Found ROM base at 0x%X", Offset);
                        return EFI_SUCCESS;
                    }
                }
            }
        }
    }

    return EFI_NOT_FOUND;
}

EFI_STATUS
VbiosFindBitHeader (
    IN  VBIOS_CONTEXT   *Context
    )
{
    UINT32  Offset;
    UINT32  EndOffset;

    if (Context->RomBase == 0) {
        return EFI_NOT_READY;
    }

    // Search for BIT signature within ROM area
    EndOffset = Context->RomBase + 0x10000;  // Search 64KB
    if (EndOffset > Context->RomSize) {
        EndOffset = (UINT32)Context->RomSize;
    }

    for (Offset = Context->RomBase; Offset < EndOffset - 12; Offset++) {
        // Check for "BIT" signature (with 0xFFB8 prefix or direct)
        if (CompareMem(Context->RomData + Offset, "BIT", 3) == 0) {
            BIT_HEADER *Hdr = (BIT_HEADER *)(Context->RomData + Offset - 2);

            // Validate header
            if (Hdr->HeaderSize > 0 && Hdr->HeaderSize < 32 &&
                Hdr->TokenSize >= 6 && Hdr->TokenSize <= 12 &&
                Hdr->TokenCount > 0 && Hdr->TokenCount < 64) {

                Context->BitHeader = Hdr;
                Context->BitTokens = (BIT_TOKEN *)(Context->RomData + Offset + 2 + Hdr->HeaderSize);
                Context->BitTokenCount = Hdr->TokenCount;

                FWSEC_DEBUG(L"Found BIT at 0x%X, %d tokens", Offset, Hdr->TokenCount);
                return EFI_SUCCESS;
            }
        }
    }

    return EFI_NOT_FOUND;
}

EFI_STATUS
VbiosGetBitToken (
    IN  VBIOS_CONTEXT   *Context,
    IN  UINT8           TokenId,
    OUT BIT_TOKEN       **Token
    )
{
    UINT32  i;
    UINT8   *TokenPtr;

    if (Context->BitHeader == NULL || Context->BitTokens == NULL) {
        return EFI_NOT_READY;
    }

    TokenPtr = (UINT8 *)Context->BitTokens;

    for (i = 0; i < Context->BitTokenCount; i++) {
        BIT_TOKEN *Tok = (BIT_TOKEN *)TokenPtr;
        if (Tok->Id == TokenId) {
            *Token = Tok;
            return EFI_SUCCESS;
        }
        if (Tok->Id == 0) {
            break;  // End of tokens
        }
        TokenPtr += Context->BitHeader->TokenSize;
    }

    return EFI_NOT_FOUND;
}

EFI_STATUS
VbiosFindPmuTable (
    IN  VBIOS_CONTEXT   *Context
    )
{
    EFI_STATUS      Status;
    BIT_TOKEN       *FalconToken;
    FALCON_DATA     *FalconData;
    UINT32          TableOffset;

    // Find FALCON_DATA token (0x70)
    Status = VbiosGetBitToken(Context, BIT_TOKEN_FALCON_DATA, &FalconToken);
    if (EFI_ERROR(Status)) {
        FWSEC_LOG(L"FALCON_DATA token not found");
        return Status;
    }

    // Get Falcon data structure
    FalconData = (FALCON_DATA *)(Context->RomData + Context->RomBase + FalconToken->DataOffset);
    TableOffset = FalconData->UcodeTableOffset;

    FWSEC_DEBUG(L"Falcon data at 0x%X, PMU table offset 0x%X",
                Context->RomBase + FalconToken->DataOffset, TableOffset);

    // The table offset might be absolute or relative
    // For Ada Lovelace, it appears to be absolute within the file
    if (TableOffset < Context->RomSize) {
        Context->PmuTable = (PMU_LOOKUP_TABLE_HEADER *)(Context->RomData + TableOffset);
    } else {
        // Try relative to ROM base
        TableOffset = Context->RomBase + FalconData->UcodeTableOffset;
        if (TableOffset >= Context->RomSize) {
            return EFI_NOT_FOUND;
        }
        Context->PmuTable = (PMU_LOOKUP_TABLE_HEADER *)(Context->RomData + TableOffset);
    }

    // Validate table header
    if (Context->PmuTable->Version != 1 ||
        Context->PmuTable->HeaderSize < 4 ||
        Context->PmuTable->EntrySize < 6 ||
        Context->PmuTable->EntryCount == 0 ||
        Context->PmuTable->EntryCount > 32) {
        FWSEC_LOG(L"Invalid PMU table header: ver=%d, hdr=%d, entry=%d, count=%d",
                  Context->PmuTable->Version, Context->PmuTable->HeaderSize,
                  Context->PmuTable->EntrySize, Context->PmuTable->EntryCount);
        return EFI_INVALID_PARAMETER;
    }

    Context->PmuEntries = (PMU_LOOKUP_TABLE_ENTRY *)(
        (UINT8 *)Context->PmuTable + Context->PmuTable->HeaderSize);
    Context->PmuEntryCount = Context->PmuTable->EntryCount;

    FWSEC_DEBUG(L"PMU table: %d entries", Context->PmuEntryCount);
    return EFI_SUCCESS;
}

EFI_STATUS
VbiosGetPmuEntry (
    IN  VBIOS_CONTEXT   *Context,
    IN  UINT8           AppId,
    OUT PMU_LOOKUP_TABLE_ENTRY **Entry
    )
{
    UINT32  i;
    UINT8   *EntryPtr;

    if (Context->PmuTable == NULL || Context->PmuEntries == NULL) {
        return EFI_NOT_READY;
    }

    EntryPtr = (UINT8 *)Context->PmuEntries;

    for (i = 0; i < Context->PmuEntryCount; i++) {
        PMU_LOOKUP_TABLE_ENTRY *Ent = (PMU_LOOKUP_TABLE_ENTRY *)EntryPtr;
        if (Ent->AppId == AppId) {
            *Entry = Ent;
            FWSEC_DEBUG(L"Found PMU entry: app=0x%02X, target=0x%02X, data=0x%X",
                        Ent->AppId, Ent->TargetId, Ent->DataOffset);
            return EFI_SUCCESS;
        }
        EntryPtr += Context->PmuTable->EntrySize;
    }

    return EFI_NOT_FOUND;
}

EFI_STATUS
VbiosExtractFwsec (
    IN  VBIOS_CONTEXT   *Context
    )
{
    EFI_STATUS                  Status;
    PMU_LOOKUP_TABLE_ENTRY      *FwsecEntry;
    UINT32                      DescOffset;

    // Find FWSEC_PROD entry
    Status = VbiosGetPmuEntry(Context, PMU_APP_ID_FWSEC_PROD, &FwsecEntry);
    if (EFI_ERROR(Status)) {
        FWSEC_LOG(L"FWSEC_PROD not found in PMU table");
        return Status;
    }

    // Calculate descriptor offset
    // The data offset in PMU entry is relative to ROM base
    DescOffset = Context->RomBase + FwsecEntry->DataOffset;
    if (DescOffset + sizeof(FALCON_UCODE_DESC_V3) > Context->RomSize) {
        FWSEC_LOG(L"FWSEC descriptor offset out of bounds: 0x%X", DescOffset);
        return EFI_INVALID_PARAMETER;
    }

    Context->FwsecDesc = (FALCON_UCODE_DESC_V3 *)(Context->RomData + DescOffset);
    Context->FwsecOffset = DescOffset;

    // Validate descriptor
    if (Context->FwsecDesc->BinHdr.VendorId != 0x10DE) {
        FWSEC_DEBUG(L"FWSEC descriptor vendor ID: 0x%04X (expected 0x10DE)",
                    Context->FwsecDesc->BinHdr.VendorId);
        // Might be encrypted - continue anyway
    }

    FWSEC_DEBUG(L"FWSEC descriptor at 0x%X", DescOffset);
    FWSEC_DEBUG(L"  IMEM: base=0x%X, size=0x%X",
                Context->FwsecDesc->ImemPhysBase, Context->FwsecDesc->ImemLoadSize);
    FWSEC_DEBUG(L"  DMEM: base=0x%X, size=0x%X",
                Context->FwsecDesc->DmemPhysBase, Context->FwsecDesc->DmemLoadSize);
    FWSEC_DEBUG(L"  Signatures: count=%d, versions=0x%04X",
                Context->FwsecDesc->SignatureCount, Context->FwsecDesc->SignatureVersions);

    return EFI_SUCCESS;
}

//==============================================================================
// FWSEC Context Implementation
//==============================================================================

EFI_STATUS
FwsecInit (
    OUT FWSEC_CONTEXT   *Context,
    IN  VBIOS_CONTEXT   *Vbios
    )
{
    if (Context == NULL || Vbios == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    ZeroMem(Context, sizeof(FWSEC_CONTEXT));
    Context->Vbios = Vbios;

    return EFI_SUCCESS;
}

EFI_STATUS
FwsecExtract (
    IN OUT FWSEC_CONTEXT *Context
    )
{
    VBIOS_CONTEXT           *Vbios = Context->Vbios;
    FALCON_UCODE_DESC_V3    *Desc;
    UINT32                  DataBase;
    UINT32                  SigSize;

    if (Vbios->FwsecDesc == NULL) {
        return EFI_NOT_READY;
    }

    Desc = Vbios->FwsecDesc;
    CopyMem(&Context->Desc, Desc, sizeof(FALCON_UCODE_DESC_V3));

    // Calculate data base (after descriptor and signatures)
    DataBase = Vbios->FwsecOffset + sizeof(FALCON_UCODE_DESC_V3);

    // Extract signatures
    SigSize = FWSEC_RSA3K_SIG_SIZE;
    Context->SignatureCount = Desc->SignatureCount;
    Context->SignatureSize = SigSize;

    if (Context->SignatureCount > 0) {
        UINT32 TotalSigSize = Context->SignatureCount * SigSize;
        Context->Signatures = AllocatePool(TotalSigSize);
        if (Context->Signatures == NULL) {
            return EFI_OUT_OF_RESOURCES;
        }
        CopyMem(Context->Signatures, Vbios->RomData + DataBase, TotalSigSize);
        DataBase += TotalSigSize;
        FWSEC_DEBUG(L"Extracted %d signatures (%d bytes each)",
                    Context->SignatureCount, SigSize);
    }

    // Extract IMEM
    Context->ImemSize = Desc->ImemLoadSize;
    if (Context->ImemSize > 0) {
        Context->ImemData = AllocatePool(Context->ImemSize);
        if (Context->ImemData == NULL) {
            return EFI_OUT_OF_RESOURCES;
        }
        CopyMem(Context->ImemData, Vbios->RomData + DataBase, Context->ImemSize);
        FWSEC_DEBUG(L"Extracted IMEM: %d bytes", Context->ImemSize);
    }

    // Extract DMEM
    Context->DmemSize = Desc->DmemLoadSize;
    if (Context->DmemSize > 0) {
        Context->DmemData = AllocatePool(Context->DmemSize);
        if (Context->DmemData == NULL) {
            return EFI_OUT_OF_RESOURCES;
        }
        CopyMem(Context->DmemData,
                Vbios->RomData + DataBase + Context->ImemSize,
                Context->DmemSize);
        FWSEC_DEBUG(L"Extracted DMEM: %d bytes", Context->DmemSize);
    }

    return EFI_SUCCESS;
}

EFI_STATUS
FwsecFindDmemMapper (
    IN OUT FWSEC_CONTEXT *Context
    )
{
    FALCON_APPIF_HDR_V1         *AppifHdr;
    FALCON_APPIF_ENTRY          *Entry;
    FALCON_APPIF_DMEMMAPPER_V3  *Mapper;
    UINT32                      InterfaceOffset;
    UINT32                      i;

    if (Context->DmemData == NULL) {
        return EFI_NOT_READY;
    }

    // Application interface is at interface_offset within DMEM
    InterfaceOffset = Context->Desc.InterfaceOffset;
    if (InterfaceOffset >= Context->DmemSize) {
        FWSEC_LOG(L"Interface offset 0x%X out of DMEM bounds", InterfaceOffset);
        return EFI_INVALID_PARAMETER;
    }

    AppifHdr = (FALCON_APPIF_HDR_V1 *)(Context->DmemData + InterfaceOffset);

    // Validate header
    if (AppifHdr->Version != 1 || AppifHdr->HeaderSize != 4 ||
        AppifHdr->EntrySize != 8 || AppifHdr->EntryCount == 0) {
        FWSEC_LOG(L"Invalid Appif header: ver=%d, hdr=%d, entry=%d, count=%d",
                  AppifHdr->Version, AppifHdr->HeaderSize,
                  AppifHdr->EntrySize, AppifHdr->EntryCount);
        return EFI_INVALID_PARAMETER;
    }

    // Find DMEMMAPPER entry
    Entry = (FALCON_APPIF_ENTRY *)(Context->DmemData + InterfaceOffset + AppifHdr->HeaderSize);

    for (i = 0; i < AppifHdr->EntryCount; i++) {
        if (Entry[i].Id == NVFW_FALCON_APPIF_ID_DMEMMAPPER) {
            Context->DmemMapperOffset = Entry[i].DmemOffset;
            Mapper = (FALCON_APPIF_DMEMMAPPER_V3 *)(Context->DmemData + Entry[i].DmemOffset);

            // Validate DMAP signature
            if (Mapper->Signature != FWSEC_DMEM_MAPPER_SIG) {
                FWSEC_LOG(L"Invalid DMEM Mapper signature: 0x%08X", Mapper->Signature);
                return EFI_INVALID_PARAMETER;
            }

            Context->DmemMapper = Mapper;
            FWSEC_DEBUG(L"Found DMEM Mapper at offset 0x%X", Entry[i].DmemOffset);
            FWSEC_DEBUG(L"  CmdIn: offset=0x%X, size=0x%X",
                        Mapper->CmdInBufferOffset, Mapper->CmdInBufferSize);
            return EFI_SUCCESS;
        }
    }

    FWSEC_LOG(L"DMEMMAPPER entry not found in Appif table");
    return EFI_NOT_FOUND;
}

EFI_STATUS
FwsecPatchFrtsCommand (
    IN OUT FWSEC_CONTEXT *Context,
    IN     FB_LAYOUT     *FbLayout
    )
{
    FWSEC_FRTS_CMD  *FrtsCmd;
    UINT32          CmdOffset;

    if (Context->DmemMapper == NULL || Context->DmemData == NULL) {
        return EFI_NOT_READY;
    }

    // Calculate command buffer offset in DMEM
    CmdOffset = Context->DmemMapperOffset + Context->DmemMapper->CmdInBufferOffset;
    if (CmdOffset + sizeof(FWSEC_FRTS_CMD) > Context->DmemSize) {
        FWSEC_LOG(L"FRTS command buffer out of DMEM bounds");
        return EFI_INVALID_PARAMETER;
    }

    FrtsCmd = (FWSEC_FRTS_CMD *)(Context->DmemData + CmdOffset);

    // Patch FRTS command
    ZeroMem(FrtsCmd, sizeof(FWSEC_FRTS_CMD));
    FrtsCmd->Cmd = FWSEC_CMD_FRTS;
    FrtsCmd->FrtsRegionOffset = (UINT32)(FbLayout->FbSize - FbLayout->FrtsBase);
    FrtsCmd->FrtsRegionSize = (UINT32)FbLayout->FrtsSize;

    FWSEC_DEBUG(L"Patched FRTS command: offset=0x%X, size=0x%X",
                FrtsCmd->FrtsRegionOffset, FrtsCmd->FrtsRegionSize);

    return EFI_SUCCESS;
}

EFI_STATUS
FwsecComputeFbLayout (
    IN  UINT32          Bar0,
    OUT FB_LAYOUT       *Layout
    )
{
    UINT32  FbSizeMb;
    UINT64  VgaBase;

    ZeroMem(Layout, sizeof(FB_LAYOUT));

    // Get usable FB size
    FbSizeMb = ReadReg32(Bar0, NV_USABLE_FB_SIZE_IN_MB) & 0xFFFF;
    Layout->FbSize = (UINT64)FbSizeMb * SIZE_1MB;
    Layout->FbUsable = Layout->FbSize;

    FWSEC_DEBUG(L"FB size: %d MB", FbSizeMb);

    // VGA workspace is at end of FB minus 1MB (PRAMIN)
    VgaBase = Layout->FbSize - SIZE_1MB;

    // Check if display is enabled for VGA workspace
    // For now, assume no VGA workspace on headless
    Layout->VgaWorkspaceBase = VgaBase;
    Layout->VgaWorkspaceSize = 0;  // Disabled

    // FRTS region is before VGA workspace, aligned to 1MB
    Layout->FrtsSize = FRTS_SIZE;
    Layout->FrtsBase = (Layout->VgaWorkspaceBase - FRTS_SIZE) & ~(FRTS_ALIGN - 1);

    FWSEC_DEBUG(L"FRTS region: 0x%lX - 0x%lX",
                Layout->FrtsBase, Layout->FrtsBase + Layout->FrtsSize);

    return EFI_SUCCESS;
}

VOID
FwsecFree (
    IN OUT FWSEC_CONTEXT *Context
    )
{
    if (Context->ImemData != NULL) {
        FreePool(Context->ImemData);
    }
    if (Context->DmemData != NULL) {
        FreePool(Context->DmemData);
    }
    if (Context->Signatures != NULL) {
        FreePool(Context->Signatures);
    }
    if (Context->DmaBuffer != NULL) {
        FreePool(Context->DmaBuffer);
    }
    ZeroMem(Context, sizeof(FWSEC_CONTEXT));
}

//==============================================================================
// Main FWSEC Execution Sequence
//==============================================================================

/**
 * Execute complete FWSEC-FRTS sequence
 * This is the main entry point similar to Linux nova-core
 */
EFI_STATUS
ExecuteFwsecFrts (
    IN  UINT32          Bar0,
    IN  UINT8           *VbiosData,
    IN  UINTN           VbiosSize
    )
{
    EFI_STATUS      Status;
    VBIOS_CONTEXT   Vbios;
    FWSEC_CONTEXT   Fwsec;
    FB_LAYOUT       FbLayout;
    FALCON_STATE    GspFalcon;
    UINT32          RomBase;
    UINT64          Wpr2Lo, Wpr2Hi;

    FWSEC_LOG(L"=== FWSEC-FRTS Execution Starting ===");

    //
    // Step 1: Wait for GFW boot to complete
    //
    FWSEC_LOG(L"Step 1: Waiting for GFW boot...");
    Status = GpuWaitGfwBoot(Bar0, GFW_BOOT_TIMEOUT_US);
    if (EFI_ERROR(Status)) {
        FWSEC_LOG(L"GFW boot timeout");
        return Status;
    }

    //
    // Step 2: Check if WPR2 already configured
    //
    FWSEC_LOG(L"Step 2: Checking WPR2 status...");
    if (GpuIsWpr2Configured(Bar0)) {
        Status = GpuReadWpr2(Bar0, &Wpr2Lo, &Wpr2Hi);
        if (!EFI_ERROR(Status)) {
            FWSEC_LOG(L"WPR2 already configured: 0x%lX - 0x%lX", Wpr2Lo, Wpr2Hi);
            return EFI_SUCCESS;  // Already done!
        }
    }

    //
    // Step 3: Parse VBIOS
    //
    FWSEC_LOG(L"Step 3: Parsing VBIOS...");
    Status = VbiosInit(&Vbios, VbiosData, VbiosSize);
    if (EFI_ERROR(Status)) {
        return Status;
    }

    Status = VbiosFindRomBase(&Vbios, &RomBase);
    if (EFI_ERROR(Status)) {
        FWSEC_LOG(L"Failed to find ROM base");
        return Status;
    }

    Status = VbiosFindBitHeader(&Vbios);
    if (EFI_ERROR(Status)) {
        FWSEC_LOG(L"Failed to find BIT header");
        return Status;
    }

    Status = VbiosFindPmuTable(&Vbios);
    if (EFI_ERROR(Status)) {
        FWSEC_LOG(L"Failed to find PMU table");
        return Status;
    }

    Status = VbiosExtractFwsec(&Vbios);
    if (EFI_ERROR(Status)) {
        FWSEC_LOG(L"Failed to extract FWSEC descriptor");
        return Status;
    }

    //
    // Step 4: Initialize FWSEC context
    //
    FWSEC_LOG(L"Step 4: Extracting FWSEC firmware...");
    Status = FwsecInit(&Fwsec, &Vbios);
    if (EFI_ERROR(Status)) {
        return Status;
    }

    Status = FwsecExtract(&Fwsec);
    if (EFI_ERROR(Status)) {
        FWSEC_LOG(L"Failed to extract FWSEC data");
        FwsecFree(&Fwsec);
        return Status;
    }

    //
    // Step 5: Find DMEM Mapper and patch FRTS command
    //
    FWSEC_LOG(L"Step 5: Patching FRTS command...");
    Status = FwsecFindDmemMapper(&Fwsec);
    if (EFI_ERROR(Status)) {
        FWSEC_LOG(L"Failed to find DMEM Mapper");
        FwsecFree(&Fwsec);
        return Status;
    }

    Status = FwsecComputeFbLayout(Bar0, &FbLayout);
    if (EFI_ERROR(Status)) {
        FwsecFree(&Fwsec);
        return Status;
    }

    Status = FwsecPatchFrtsCommand(&Fwsec, &FbLayout);
    if (EFI_ERROR(Status)) {
        FWSEC_LOG(L"Failed to patch FRTS command");
        FwsecFree(&Fwsec);
        return Status;
    }

    //
    // Step 6: Initialize GSP Falcon
    //
    FWSEC_LOG(L"Step 6: Initializing GSP Falcon...");
    Status = FalconInit(&GspFalcon, Bar0, FALCON_GSP_BASE);
    if (EFI_ERROR(Status)) {
        FwsecFree(&Fwsec);
        return Status;
    }

    //
    // Step 7: Reset Falcon
    //
    FWSEC_LOG(L"Step 7: Resetting Falcon...");
    Status = FalconReset(Bar0, &GspFalcon);
    if (EFI_ERROR(Status)) {
        FWSEC_LOG(L"Falcon reset failed");
        FwsecFree(&Fwsec);
        return Status;
    }

    //
    // Step 8: Load FWSEC via DMA
    //
    FWSEC_LOG(L"Step 8: Loading FWSEC via DMA...");
    // TODO: Implement DMA loading with proper signature patching
    // This requires:
    // 1. Allocate DMA-capable buffer
    // 2. Copy IMEM + DMEM + patched signature
    // 3. Configure FBIF for system memory
    // 4. Use BROM interface to load and verify

    //
    // Step 9: Execute FWSEC via BROM
    //
    FWSEC_LOG(L"Step 9: Executing FWSEC via BROM...");
    // TODO: Implement BROM execution
    // This requires proper BROM register programming

    //
    // Step 10: Check results
    //
    FWSEC_LOG(L"Step 10: Checking results...");

    // Check FRTS error code
    UINT16 FrtsErr = GpuGetFrtsErrorCode(Bar0);
    if (FrtsErr != FRTS_ERR_NONE) {
        FWSEC_LOG(L"FRTS error code: 0x%04X", FrtsErr);
        FwsecFree(&Fwsec);
        return EFI_DEVICE_ERROR;
    }

    // Check WPR2
    Status = GpuReadWpr2(Bar0, &Wpr2Lo, &Wpr2Hi);
    if (EFI_ERROR(Status) || Wpr2Hi == 0) {
        FWSEC_LOG(L"WPR2 not configured after FWSEC execution");
        FwsecFree(&Fwsec);
        return EFI_DEVICE_ERROR;
    }

    FWSEC_LOG(L"=== FWSEC-FRTS Success ===");
    FWSEC_LOG(L"WPR2: 0x%lX - 0x%lX", Wpr2Lo, Wpr2Hi);

    FwsecFree(&Fwsec);
    return EFI_SUCCESS;
}

//==============================================================================
// GPU Helper Functions Implementation
//==============================================================================

EFI_STATUS
GpuWaitGfwBoot (
    IN  UINT32          Bar0,
    IN  UINTN           TimeoutUs
    )
{
    UINTN   Elapsed = 0;
    UINT32  Progress;

    while (Elapsed < TimeoutUs) {
        Progress = ReadReg32(Bar0, NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_0);
        if ((Progress & 0xFF) == GFW_BOOT_PROGRESS_COMPLETED) {
            FWSEC_DEBUG(L"GFW boot completed (progress=0x%X)", Progress);
            return EFI_SUCCESS;
        }

        // Wait 1ms
        gBS->Stall(1000);
        Elapsed += 1000;
    }

    FWSEC_LOG(L"GFW boot timeout (progress=0x%X)", Progress);
    return EFI_TIMEOUT;
}

BOOLEAN
GpuIsWpr2Configured (
    IN  UINT32          Bar0
    )
{
    UINT32  Wpr2Hi = ReadReg32(Bar0, NV_PFB_PRI_MMU_WPR2_ADDR_HI);
    return (Wpr2Hi & 0xFFFFFFF0) != 0;
}

EFI_STATUS
GpuReadWpr2 (
    IN  UINT32          Bar0,
    OUT UINT64          *Wpr2Lo,
    OUT UINT64          *Wpr2Hi
    )
{
    UINT32  Lo = ReadReg32(Bar0, NV_PFB_PRI_MMU_WPR2_ADDR_LO);
    UINT32  Hi = ReadReg32(Bar0, NV_PFB_PRI_MMU_WPR2_ADDR_HI);

    *Wpr2Lo = ((UINT64)(Lo & 0xFFFFFFF0)) << 8;  // Shift by 12, but register has bits 31:4
    *Wpr2Hi = ((UINT64)(Hi & 0xFFFFFFF0)) << 8;

    return EFI_SUCCESS;
}

UINT16
GpuGetFrtsErrorCode (
    IN  UINT32          Bar0
    )
{
    UINT32  Scratch = ReadReg32(Bar0, NV_PBUS_SW_SCRATCH_0E);
    return (UINT16)((Scratch >> 16) & 0xFFFF);
}

UINT64
GpuGetUsableFbSize (
    IN  UINT32          Bar0
    )
{
    UINT32  SizeMb = ReadReg32(Bar0, NV_USABLE_FB_SIZE_IN_MB) & 0xFFFF;
    return (UINT64)SizeMb * SIZE_1MB;
}

UINT8
GpuGetArchitecture (
    IN  UINT32          Bar0
    )
{
    UINT32  Boot0 = ReadReg32(Bar0, NV_PMC_BOOT_0);
    return (UINT8)((Boot0 >> NV_PMC_BOOT_0_ARCH_SHIFT) & 0x1F);
}

//==============================================================================
// Falcon Functions Implementation
//==============================================================================

EFI_STATUS
FalconInit (
    OUT FALCON_STATE    *State,
    IN  UINT32          Bar0,
    IN  UINT32          FalconBase
    )
{
    UINT32  Hwcfg2;

    ZeroMem(State, sizeof(FALCON_STATE));
    State->Base = FalconBase;
    State->IsGsp = (FalconBase == FALCON_GSP_BASE);

    // Check RISC-V capability
    Hwcfg2 = FalconReadReg(Bar0, FalconBase, FALCON_HWCFG2);
    State->IsRiscV = (Hwcfg2 & FALCON_HWCFG2_RISCV) != 0;

    // Check if halted
    UINT32 Cpuctl = FalconReadReg(Bar0, FalconBase, FALCON_CPUCTL);
    State->Halted = (Cpuctl & FALCON_CPUCTL_HALTED) != 0;

    FWSEC_DEBUG(L"Falcon init: base=0x%X, RISC-V=%d, halted=%d",
                FalconBase, State->IsRiscV, State->Halted);

    return EFI_SUCCESS;
}

EFI_STATUS
FalconReset (
    IN  UINT32          Bar0,
    IN  FALCON_STATE    *State
    )
{
    UINT32  FalconBase = State->Base;
    UINT32  Hwcfg2;
    UINTN   Timeout = 100000;  // 100ms

    // Wait for memory scrub to complete if in progress
    do {
        Hwcfg2 = FalconReadReg(Bar0, FalconBase, FALCON_HWCFG2);
        if ((Hwcfg2 & FALCON_HWCFG2_MEM_SCRUBBING) == 0) {
            break;
        }
        gBS->Stall(100);
        Timeout -= 100;
    } while (Timeout > 0);

    if (Timeout == 0) {
        FWSEC_LOG(L"Falcon memory scrub timeout");
    }

    // Select Falcon core (for dual-controller)
    UINT32 BcrCtrl = FalconReadReg(Bar0, FalconBase, FALCON_BCR_CTRL);
    if (BcrCtrl != FALCON_BCR_CTRL_CORE_SELECT) {
        FalconWriteReg(Bar0, FalconBase, FALCON_BCR_CTRL, FALCON_BCR_CTRL_CORE_SELECT);

        // Wait for core select
        Timeout = 10000;
        do {
            BcrCtrl = FalconReadReg(Bar0, FalconBase, FALCON_BCR_CTRL);
            if (BcrCtrl == FALCON_BCR_CTRL_CORE_SELECT) {
                break;
            }
            gBS->Stall(10);
            Timeout -= 10;
        } while (Timeout > 0);
    }

    State->Halted = TRUE;
    FWSEC_DEBUG(L"Falcon reset complete");

    return EFI_SUCCESS;
}

EFI_STATUS
FalconReadMailbox (
    IN  UINT32          Bar0,
    IN  FALCON_STATE    *State,
    OUT UINT32          *Mbox0,
    OUT UINT32          *Mbox1
    )
{
    *Mbox0 = FalconReadReg(Bar0, State->Base, FALCON_MAILBOX0);
    *Mbox1 = FalconReadReg(Bar0, State->Base, FALCON_MAILBOX1);
    State->MailBox0 = *Mbox0;
    State->MailBox1 = *Mbox1;
    return EFI_SUCCESS;
}
