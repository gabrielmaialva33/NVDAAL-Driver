/** @file
  NVDAAL FWSEC Executor - EFI Driver to execute NVIDIA FWSEC-FRTS

  This driver parses the NVIDIA VBIOS, extracts FWSEC firmware,
  and executes it on the GSP Falcon to configure WPR2.

  Based on Linux nouveau/nova-core FWSEC implementation.

  Copyright (c) 2024, NVDAAL Project. All rights reserved.
  SPDX-License-Identifier: MIT
**/

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/PrintLib.h>
#include <Protocol/PciIo.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/LoadedImage.h>
#include <Guid/FileInfo.h>
#include <IndustryStandard/Pci.h>

// Forward declaration of new FWSEC implementation
EFI_STATUS
FwsecExecuteFrts (
    IN  UINT32  Bar0,
    IN  UINT8   *VbiosData,
    IN  UINTN   VbiosSize,
    IN  UINT64  FrtsOffset
    );

//=============================================================================
// Constants
//=============================================================================

// NVIDIA Vendor ID and Device IDs
#define NVIDIA_VENDOR_ID          0x10DE
#define NVIDIA_RTX4090_ID1        0x2684
#define NVIDIA_RTX4090_ID2        0x2685
#define NVIDIA_RTX4080_ID1        0x2702
#define NVIDIA_RTX4070TI_ID1      0x2782

// GPU Register Offsets
#define NV_PMC_BOOT_0                   0x00000000
#define NV_PFB_PRI_MMU_WPR2_ADDR_LO     0x001FA820
#define NV_PFB_PRI_MMU_WPR2_ADDR_HI     0x001FA824

// GSP Falcon Base and Registers
#define NV_PGSP_BASE                    0x00110000
#define FALCON_IRQSSET                  0x0000
#define FALCON_IRQSCLR                  0x0004
#define FALCON_IRQMSET                  0x0010
#define FALCON_IRQMCLR                  0x0014
#define FALCON_IRQDEST                  0x001C
#define FALCON_MAILBOX0                 0x0040
#define FALCON_MAILBOX1                 0x0044
#define FALCON_ITFEN                    0x0048
#define FALCON_CPUCTL                   0x0100
#define FALCON_BOOTVEC                  0x0104
#define FALCON_HWCFG                    0x0108
#define FALCON_DMACTL                   0x010C
#define FALCON_DMATRFBASE               0x0110
#define FALCON_DMATRFMOFFS              0x0114
#define FALCON_DMATRFCMD                0x0118
#define FALCON_IMEMC(i)                 (0x0180 + (i) * 16)
#define FALCON_IMEMD(i)                 (0x0184 + (i) * 16)
#define FALCON_DMEMC(i)                 (0x01C0 + (i) * 8)
#define FALCON_DMEMD(i)                 (0x01C4 + (i) * 8)

// Falcon Control Bits
#define FALCON_CPUCTL_STARTCPU          (1 << 1)
#define FALCON_CPUCTL_HALTED            (1 << 4)

// Falcon Memory Control Bits
#define FALCON_IMEMC_AINCW              (1 << 24)  // Auto-increment on write
#define FALCON_DMEMC_AINCW              (1 << 24)  // Auto-increment on write
#define FALCON_MEM_WRITE_ENABLE         (1 << 23)  // Write enable bit (CRITICAL: bit 23, NOT 24)

// PMC (Power Management Controller) Registers
#define NV_PMC_ENABLE                   0x00000200
#define NV_PMC_DEVICE_ENABLE            0x00000600

// PCI Power Management
#define PCI_PM_CAP_ID                   0x01
#define PCI_PM_CTRL                     0x04       // Offset from PM capability
#define PCI_PM_CTRL_STATE_MASK          0x0003
#define PCI_PM_CTRL_D0                  0x0000
#define PCI_PM_CTRL_D3HOT               0x0003

// GPU Reset Registers
#define NV_PMC_INTR_EN_0                0x00000140
#define NV_PBUS_PCI_NV_19               0x0000184C  // Secondary Bus Reset

// BROM (Boot ROM) Parameter Registers - For HS mode signature verification
// These are the NV_PFALCON2_* registers at offsets 0x1180+ from Falcon base
#define FALCON_BROM_MOD_SEL             0x1180      // Algorithm selection (RSA3K = 1)
#define FALCON_BROM_CURR_UCODE_ID       0x1198      // Current ucode ID
#define FALCON_BROM_ENGIDMASK           0x119C      // Engine ID mask
#define FALCON_BROM_PARAADDR            0x1210      // PKC data offset (signature offset in DMEM)

// DMA Transfer Registers - Used to load firmware into IMEM/DMEM
#define FALCON_DMATRFBASE               0x0110      // DMA base address (low bits >> 8)
#define FALCON_DMATRFMOFFS              0x0114      // Memory offset in Falcon
#define FALCON_DMATRFCMD                0x0118      // DMA transfer command
#define FALCON_DMATRFFBOFFS             0x011C      // Framebuffer/source offset
#define FALCON_DMATRFBASE1              0x0128      // DMA base address (high bits)

// DMATRFCMD bits
#define DMATRFCMD_IDLE                  (1 << 1)    // Transfer idle
#define DMATRFCMD_IMEM                  (1 << 4)    // Target is IMEM (else DMEM)
#define DMATRFCMD_SIZE_256B             (6 << 8)    // 256-byte transfer size

// FBIF (Framebuffer Interface) Registers - Controls DMA access to external memory
#define FALCON_FBIF_TRANSCFG            0x0600      // Transfer config (target, mem type)
#define FALCON_FBIF_CTL                 0x0624      // FBIF control

// FBIF TRANSCFG bits
#define FBIF_TRANSCFG_TARGET_COHERENT   0x01        // Coherent system memory
#define FBIF_TRANSCFG_TARGET_NONCOHERENT 0x02       // Non-coherent system memory
#define FBIF_TRANSCFG_MEM_PHYSICAL      (1 << 2)    // Physical addresses

// Additional Falcon registers
#define FALCON_HWCFG1                   0x012C      // Hardware config 1
#define FALCON_HWCFG2                   0x00F4      // Hardware config 2
#define FALCON_ENGINE                   0x03C0      // Engine reset control
#define FALCON_RM                       0x0084      // RM register (set to PMC_BOOT_0)

// RISCV/Falcon dual-core control (for Peregrine controllers)
#define FALCON_RISCV_BCR_CTRL           0x1668      // Core select control

// Memory flush registers
#define NV_PBUS_FBIO_FLUSH              0x00001220  // FBIO flush

// VBIOS Offsets
#define VBIOS_ROM_OFFSET                0x300000

// Utility macros
#ifndef MIN
#define MIN(a, b)  (((a) < (b)) ? (a) : (b))
#endif

// BIT Table
#define BIT_HEADER_ID                   0xB8FF
#define BIT_HEADER_SIGNATURE            0x00544942  // "BIT\0"
#define BIT_TOKEN_FALCON_DATA           0x70

// PMU App IDs
#define FWSEC_APP_ID_FRTS               0x01
#define FWSEC_APP_ID_FWSEC              0x85

// DMEMMAPPER
#define DMEMMAPPER_SIGNATURE            0x50414D44  // "DMAP"
#define DMEMMAPPER_CMD_FRTS             0x15

//=============================================================================
// Data Structures
//=============================================================================

#pragma pack(1)

// VBIOS ROM Header
typedef struct {
  UINT16  Signature;          // 0x55AA
  UINT8   Reserved[0x16];
  UINT16  PcirOffset;
} VBIOS_ROM_HEADER;

// PCI Data Structure
typedef struct {
  UINT32  Signature;          // "PCIR"
  UINT16  VendorId;
  UINT16  DeviceId;
  UINT16  Reserved;
  UINT16  Length;
  UINT8   Revision;
  UINT8   ClassCode[3];
  UINT16  ImageLength;        // In 512-byte units
  UINT16  CodeRevision;
  UINT8   CodeType;           // 0x00=PCI-AT, 0x03=EFI, 0xE0=FWSEC
  UINT8   Indicator;          // Bit 7 = last image
  UINT16  Reserved2;
} VBIOS_PCIR_HEADER;

// BIT Header
typedef struct {
  UINT16  Id;                 // 0xB8FF
  UINT32  Signature;          // "BIT\0"
  UINT16  BcdVersion;
  UINT8   HeaderSize;
  UINT8   TokenSize;
  UINT8   TokenCount;
  UINT8   Checksum;
} BIT_HEADER;

// BIT Token
typedef struct {
  UINT8   Id;
  UINT8   DataVersion;
  UINT16  DataSize;
  UINT16  DataOffset;         // Relative to image base
} BIT_TOKEN;

// BIT Falcon Data
typedef struct {
  UINT32  UcodeTableOffset;   // Offset to PMU Lookup Table
} BIT_FALCON_DATA;

// PMU Lookup Table Header
typedef struct {
  UINT8   Version;
  UINT8   HeaderSize;
  UINT8   EntrySize;
  UINT8   EntryCount;
} PMU_LOOKUP_TABLE_HEADER;

// PMU Lookup Entry
typedef struct {
  UINT8   AppId;
  UINT8   TargetId;
  UINT32  DataOffset;
} PMU_LOOKUP_ENTRY;

// Falcon Ucode Descriptor V3
typedef struct {
  UINT32  StoredSize;
  UINT32  ImemOffset;
  UINT32  ImemLoadSize;
  UINT32  ImemSecureSize;
  UINT32  DmemOffset;
  UINT32  DmemSize;
  UINT32  Reserved1;
  UINT32  Reserved2;
  UINT32  BootVec;
  UINT32  SigOffset;
  UINT32  SigSize;
  UINT32  Reserved3;
} FALCON_UCODE_DESC_V3;

// DMEMMAPPER Header
typedef struct {
  UINT32  Signature;          // "DMAP"
  UINT16  Version;
  UINT16  Size;
  UINT32  CmdBufOffset;
  UINT32  CmdBufSize;
  UINT32  InitCmd;            // Command to execute (set to 0x15 for FRTS)
  // ... more fields follow
} DMEMMAPPER_HEADER;

// FWSEC Info (extracted from VBIOS)
typedef struct {
  BOOLEAN Valid;
  UINT32  ImemOffset;
  UINT32  ImemSize;
  UINT32  ImemSecSize;
  UINT32  DmemOffset;
  UINT32  DmemSize;
  UINT32  BootVec;
  UINT32  DmemMapperOffset;   // Offset within DMEM
  UINT32  SigOffset;          // Signature offset for BROM verification
  UINT32  SigSize;            // Signature size
  UINT32  StoredSize;         // Total stored size of firmware blob
} FWSEC_INFO;

// DMA Buffer Info (for BROM interface)
typedef struct {
  UINT8        *VirtualAddr;   // CPU-visible address
  EFI_PHYSICAL_ADDRESS PhysAddr; // Physical/DMA address
  UINTN        Size;           // Buffer size
  BOOLEAN      Valid;
} DMA_BUFFER;

#pragma pack()

//=============================================================================
// Global Variables
//=============================================================================

STATIC EFI_PCI_IO_PROTOCOL  *mPciIo = NULL;
STATIC UINT32               *mMmioBase = NULL;
STATIC FWSEC_INFO           mFwsecInfo;
STATIC EFI_FILE_PROTOCOL    *mLogFile = NULL;
STATIC EFI_FILE_PROTOCOL    *mLogRoot = NULL;  // Root directory for saving additional files
STATIC CHAR8                mLogBuffer[512];

//=============================================================================
// File Logging
//=============================================================================

STATIC
EFI_STATUS
OpenLogFile (
  IN EFI_HANDLE  ImageHandle
  )
{
  EFI_STATUS                       Status;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *FileSystem;
  EFI_FILE_PROTOCOL                *Root;
  EFI_HANDLE                       *HandleBuffer;
  UINTN                            HandleCount;
  UINTN                            Index;

  (VOID)ImageHandle;  // Unused

  // Find all file system handles
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleFileSystemProtocolGuid,
                  NULL,
                  &HandleCount,
                  &HandleBuffer
                  );
  if (EFI_ERROR (Status) || HandleCount == 0) {
    Print (L"NVDAAL: No file systems found\n");
    return EFI_NOT_FOUND;
  }

  Print (L"NVDAAL: Found %u file system(s)\n", HandleCount);

  // Try each file system
  for (Index = 0; Index < HandleCount; Index++) {
    Status = gBS->HandleProtocol (
                    HandleBuffer[Index],
                    &gEfiSimpleFileSystemProtocolGuid,
                    (VOID **)&FileSystem
                    );
    if (EFI_ERROR (Status)) {
      continue;
    }

    Status = FileSystem->OpenVolume (FileSystem, &Root);
    if (EFI_ERROR (Status)) {
      continue;
    }

    // Try to create log file in EFI\OC directory
    Status = Root->Open (
                     Root,
                     &mLogFile,
                     L"\\EFI\\OC\\NVDAAL_LOG.txt",
                     EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                     0
                     );
    if (!EFI_ERROR (Status)) {
      Print (L"NVDAAL: Log file created in EFI\\OC on FS %u\n", Index);
      mLogRoot = Root;  // Keep root open for saving additional files
      FreePool (HandleBuffer);
      return EFI_SUCCESS;
    }

    // Try root directory
    Status = Root->Open (
                     Root,
                     &mLogFile,
                     L"\\NVDAAL_LOG.txt",
                     EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                     0
                     );
    if (!EFI_ERROR (Status)) {
      Print (L"NVDAAL: Log file created in root on FS %u\n", Index);
      mLogRoot = Root;  // Keep root open for saving additional files
      FreePool (HandleBuffer);
      return EFI_SUCCESS;
    }

    Root->Close (Root);
  }

  FreePool (HandleBuffer);
  Print (L"NVDAAL: Cannot create log file on any FS\n");
  return EFI_NOT_FOUND;
}

STATIC
VOID
CloseLogFile (
  VOID
  )
{
  if (mLogFile != NULL) {
    mLogFile->Close (mLogFile);
    mLogFile = NULL;
  }
  if (mLogRoot != NULL) {
    mLogRoot->Close (mLogRoot);
    mLogRoot = NULL;
  }
}

// Forward declaration (not STATIC so fwsec_impl.c can use it)
VOID LogPrint (IN CONST CHAR16 *Format, ...);

//=============================================================================
// Scrubber Firmware Loading
//=============================================================================

// Scrubber firmware header (from NVIDIA open-gpu-kernel-modules)
typedef struct {
  UINT16  VendorId;        // 0x10DE
  UINT16  Reserved;
  UINT32  Version;
  UINT32  Size;            // Firmware size
  UINT32  HeaderSize;      // This header size
  UINT32  ImemSize;        // IMEM size
  UINT32  ImemOffset;      // IMEM offset from header
  UINT32  DmemSize;        // DMEM size
  UINT32  DmemOffset;      // DMEM offset from header
  // More fields follow...
} SCRUBBER_HEADER;

STATIC UINT8  *mScrubberData = NULL;
STATIC UINTN  mScrubberSize = 0;

STATIC
EFI_STATUS
LoadScrubberFirmware (
  VOID
  )
{
  EFI_STATUS                       Status;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *FileSystem;
  EFI_FILE_PROTOCOL                *Root;
  EFI_FILE_PROTOCOL                *File;
  EFI_HANDLE                       *HandleBuffer;
  UINTN                            HandleCount;
  UINTN                            Index;
  EFI_FILE_INFO                    *FileInfo;
  UINTN                            FileInfoSize;
  UINT8                            FileInfoBuffer[256];
  SCRUBBER_HEADER                  *Hdr;

  LogPrint (L"NVDAAL: Loading scrubber firmware from EFI partition...\n");

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleFileSystemProtocolGuid,
                  NULL,
                  &HandleCount,
                  &HandleBuffer
                  );
  if (EFI_ERROR (Status) || HandleCount == 0) {
    LogPrint (L"NVDAAL: No file systems found for firmware\n");
    return EFI_NOT_FOUND;
  }

  for (Index = 0; Index < HandleCount; Index++) {
    Status = gBS->HandleProtocol (
                    HandleBuffer[Index],
                    &gEfiSimpleFileSystemProtocolGuid,
                    (VOID **)&FileSystem
                    );
    if (EFI_ERROR (Status)) continue;

    Status = FileSystem->OpenVolume (FileSystem, &Root);
    if (EFI_ERROR (Status)) continue;

    // Try to open scrubber.bin
    Status = Root->Open (
                     Root,
                     &File,
                     L"\\EFI\\OC\\NVDAAL\\scrubber.bin",
                     EFI_FILE_MODE_READ,
                     0
                     );
    if (EFI_ERROR (Status)) {
      Root->Close (Root);
      continue;
    }

    LogPrint (L"NVDAAL: Found scrubber.bin on FS %u\n", Index);

    // Get file size
    FileInfoSize = sizeof(FileInfoBuffer);
    Status = File->GetInfo (File, &gEfiFileInfoGuid, &FileInfoSize, FileInfoBuffer);
    if (EFI_ERROR (Status)) {
      LogPrint (L"NVDAAL: Cannot get file info\n");
      File->Close (File);
      Root->Close (Root);
      continue;
    }

    FileInfo = (EFI_FILE_INFO *)FileInfoBuffer;
    mScrubberSize = (UINTN)FileInfo->FileSize;
    LogPrint (L"NVDAAL: Scrubber size: %u bytes\n", mScrubberSize);

    // Allocate buffer
    mScrubberData = AllocatePool (mScrubberSize);
    if (mScrubberData == NULL) {
      LogPrint (L"NVDAAL: Cannot allocate scrubber buffer\n");
      File->Close (File);
      Root->Close (Root);
      FreePool (HandleBuffer);
      return EFI_OUT_OF_RESOURCES;
    }

    // Read file
    Status = File->Read (File, &mScrubberSize, mScrubberData);
    File->Close (File);
    Root->Close (Root);

    if (EFI_ERROR (Status)) {
      LogPrint (L"NVDAAL: Failed to read scrubber: %r\n", Status);
      FreePool (mScrubberData);
      mScrubberData = NULL;
      continue;
    }

    // Validate header
    Hdr = (SCRUBBER_HEADER *)mScrubberData;
    LogPrint (L"NVDAAL: Scrubber header: VendorId=0x%04X, Size=0x%X, HeaderSize=0x%X\n",
              Hdr->VendorId, Hdr->Size, Hdr->HeaderSize);
    LogPrint (L"NVDAAL: IMEM: offset=0x%X, size=0x%X\n", Hdr->ImemOffset, Hdr->ImemSize);
    LogPrint (L"NVDAAL: DMEM: offset=0x%X, size=0x%X\n", Hdr->DmemOffset, Hdr->DmemSize);

    if (Hdr->VendorId == 0x10DE) {
      LogPrint (L"NVDAAL: Scrubber firmware loaded successfully!\n");
      FreePool (HandleBuffer);
      return EFI_SUCCESS;
    } else {
      LogPrint (L"NVDAAL: Invalid scrubber header (VendorId mismatch)\n");
      FreePool (mScrubberData);
      mScrubberData = NULL;
    }
  }

  FreePool (HandleBuffer);
  LogPrint (L"NVDAAL: Scrubber firmware not found\n");
  return EFI_NOT_FOUND;
}

VOID
LogPrint (
  IN CONST CHAR16  *Format,
  ...
  )
{
  VA_LIST  Args;
  CHAR16   Buffer[256];
  UINTN    Len;
  UINTN    i;

  // Format the message
  VA_START (Args, Format);
  UnicodeVSPrint (Buffer, sizeof(Buffer), Format, Args);
  VA_END (Args);

  // Print to console
  Print (L"%s", Buffer);

  // Write to log file if open
  if (mLogFile != NULL) {
    // Convert to ASCII for log file
    Len = StrLen (Buffer);
    for (i = 0; i < Len && i < sizeof(mLogBuffer) - 1; i++) {
      mLogBuffer[i] = (CHAR8)Buffer[i];
    }
    mLogBuffer[i] = '\0';

    // Write to file
    UINTN WriteSize = i;
    mLogFile->Write (mLogFile, &WriteSize, mLogBuffer);
    mLogFile->Flush (mLogFile);
  }
}

// Simple string logging function (non-variadic, can be called from other files)
VOID
LogStr (
  IN CONST CHAR16  *Str
  )
{
  LogPrint (L"%s", Str);
}

//=============================================================================
// Register Access
//=============================================================================

STATIC
UINT32
ReadReg (
  IN UINT32  Offset
  )
{
  UINT32 Value = 0xDEADDEAD;

  // Use PciIo protocol for proper MMIO access
  if (mPciIo != NULL) {
    mPciIo->Mem.Read (
      mPciIo,
      EfiPciIoWidthUint32,
      0,  // BAR0
      Offset,
      1,
      &Value
      );
  }
  return Value;
}

STATIC
VOID
WriteReg (
  IN UINT32  Offset,
  IN UINT32  Value
  )
{
  // Use PciIo protocol for proper MMIO access
  if (mPciIo != NULL) {
    mPciIo->Mem.Write (
      mPciIo,
      EfiPciIoWidthUint32,
      0,  // BAR0
      Offset,
      1,
      &Value
      );
    MemoryFence ();
  }
}

STATIC
BOOLEAN
IsWpr2Enabled (
  VOID
  )
{
  UINT32  Wpr2Hi = ReadReg (NV_PFB_PRI_MMU_WPR2_ADDR_HI);
  return (Wpr2Hi >> 31) & 1;
}

//=============================================================================
// GPU Power Cycle (ACPI-style D3/D0 transition)
//=============================================================================

STATIC
UINT8
FindPciCapability (
  IN EFI_PCI_IO_PROTOCOL  *PciIo,
  IN UINT8                CapId
  )
{
  UINT8   CapPtr;
  UINT8   CapIdFound;
  UINT8   NextPtr;
  UINTN   Count;

  // Read capabilities pointer from PCI config space (offset 0x34)
  PciIo->Pci.Read (PciIo, EfiPciIoWidthUint8, 0x34, 1, &CapPtr);

  if (CapPtr == 0 || CapPtr == 0xFF) {
    return 0;
  }

  // Walk the capability list
  for (Count = 0; Count < 48; Count++) {
    PciIo->Pci.Read (PciIo, EfiPciIoWidthUint8, CapPtr, 1, &CapIdFound);

    if (CapIdFound == CapId) {
      return CapPtr;
    }

    PciIo->Pci.Read (PciIo, EfiPciIoWidthUint8, CapPtr + 1, 1, &NextPtr);

    if (NextPtr == 0) {
      break;
    }

    CapPtr = NextPtr;
  }

  return 0;
}

STATIC
EFI_STATUS
GpuPowerCycle (
  VOID
  )
{
  UINT8   PmCapOffset;
  UINT16  PmCtrl;
  UINT32  PmcEnable;
  UINT32  Boot0;

  if (mPciIo == NULL) {
    Print (L"NVDAAL: PciIo not available for power cycle\n");
    return EFI_NOT_READY;
  }

  LogPrint (L"\n=== GPU Power Cycle (Cold Boot Trigger) ===\n");

  // Check current Boot ROM state
  UINT32 BromEngctl = ReadReg (NV_PGSP_BASE + 0xA0);  // BROM_ENGCTL approx
  UINT32 BromParamOff = ReadReg (NV_PGSP_BASE + 0xA4);
  LogPrint (L"NVDAAL: Current BROM state: ENGCTL=0x%08X, PARAM=0x%08X\n", BromEngctl, BromParamOff);

  // Check if FWSEC already executed by looking at specific registers
  UINT32 FbMemUnlock = ReadReg (0x00100C64);  // FB_MEM_UNLOCK
  LogPrint (L"NVDAAL: FB_MEM_UNLOCK = 0x%08X\n", FbMemUnlock);

  // Method 1: PCI Power Management D3/D0 Transition
  LogPrint (L"NVDAAL: Attempting PCI D3/D0 power state transition...\n");

  PmCapOffset = FindPciCapability (mPciIo, PCI_PM_CAP_ID);

  if (PmCapOffset != 0) {
    Print (L"NVDAAL: Found PM capability at offset 0x%02X\n", PmCapOffset);

    // Read current PM state
    mPciIo->Pci.Read (mPciIo, EfiPciIoWidthUint16, PmCapOffset + PCI_PM_CTRL, 1, &PmCtrl);
    Print (L"NVDAAL: Current PM Control: 0x%04X (State: D%d)\n", PmCtrl, PmCtrl & PCI_PM_CTRL_STATE_MASK);

    // Transition to D3hot (power off)
    Print (L"NVDAAL: Transitioning to D3hot...\n");
    PmCtrl = (PmCtrl & ~PCI_PM_CTRL_STATE_MASK) | PCI_PM_CTRL_D3HOT;
    mPciIo->Pci.Write (mPciIo, EfiPciIoWidthUint16, PmCapOffset + PCI_PM_CTRL, 1, &PmCtrl);

    // Wait 100ms in D3
    gBS->Stall (100000);

    // Transition back to D0 (power on)
    Print (L"NVDAAL: Transitioning to D0...\n");
    PmCtrl = (PmCtrl & ~PCI_PM_CTRL_STATE_MASK) | PCI_PM_CTRL_D0;
    mPciIo->Pci.Write (mPciIo, EfiPciIoWidthUint16, PmCapOffset + PCI_PM_CTRL, 1, &PmCtrl);

    // Wait 200ms for GPU to reinitialize
    Print (L"NVDAAL: Waiting for GPU to reinitialize...\n");
    gBS->Stall (200000);

    // Read back PM state
    mPciIo->Pci.Read (mPciIo, EfiPciIoWidthUint16, PmCapOffset + PCI_PM_CTRL, 1, &PmCtrl);
    Print (L"NVDAAL: New PM Control: 0x%04X (State: D%d)\n", PmCtrl, PmCtrl & PCI_PM_CTRL_STATE_MASK);
  } else {
    Print (L"NVDAAL: PM capability not found, trying PMC reset...\n");
  }

  // Method 2: PMC Full Reset (disable all engines, then re-enable)
  Print (L"NVDAAL: Performing PMC engine reset...\n");

  // Save current PMC_ENABLE
  PmcEnable = ReadReg (NV_PMC_ENABLE);
  Print (L"NVDAAL: Current PMC_ENABLE: 0x%08X\n", PmcEnable);

  // Disable all engines
  Print (L"NVDAAL: Disabling all GPU engines...\n");
  WriteReg (NV_PMC_ENABLE, 0);
  gBS->Stall (50000);  // 50ms

  // Re-enable all engines
  Print (L"NVDAAL: Re-enabling GPU engines...\n");
  WriteReg (NV_PMC_ENABLE, 0xFFFFFFFF);
  gBS->Stall (100000);  // 100ms

  // Wait for GPU to stabilize
  Print (L"NVDAAL: Waiting for GPU stabilization...\n");
  gBS->Stall (500000);  // 500ms

  // Check if GPU is responding
  Boot0 = ReadReg (NV_PMC_BOOT_0);
  Print (L"NVDAAL: PMC_BOOT_0 after reset: 0x%08X\n", Boot0);

  if (Boot0 == 0 || Boot0 == 0xFFFFFFFF) {
    Print (L"NVDAAL: WARNING - GPU may not have recovered properly\n");
    // Try to restore PMC_ENABLE
    WriteReg (NV_PMC_ENABLE, PmcEnable);
    gBS->Stall (100000);
  }

  // Check WPR2 status after power cycle
  Print (L"NVDAAL: Checking WPR2 after power cycle...\n");
  if (IsWpr2Enabled ()) {
    Print (L"NVDAAL: *** WPR2 is now ENABLED after power cycle! ***\n");
    return EFI_SUCCESS;
  }

  Print (L"NVDAAL: WPR2 still not enabled after power cycle\n");
  return EFI_NOT_READY;
}

STATIC
VOID
PrintGpuStatus (
  IN CONST CHAR16  *Label
  )
{
  UINT32  Boot0, Wpr2Lo, Wpr2Hi, GspCpuctl;

  Boot0 = ReadReg (NV_PMC_BOOT_0);
  Wpr2Lo = ReadReg (NV_PFB_PRI_MMU_WPR2_ADDR_LO);
  Wpr2Hi = ReadReg (NV_PFB_PRI_MMU_WPR2_ADDR_HI);
  GspCpuctl = ReadReg (NV_PGSP_BASE + FALCON_CPUCTL);

  LogPrint (L"%s:\n", Label);
  LogPrint (L"  PMC_BOOT_0    = 0x%08X (Arch: 0x%02X)\n", Boot0, (Boot0 >> 20) & 0xFF);
  LogPrint (L"  WPR2_LO       = 0x%08X\n", Wpr2Lo);
  LogPrint (L"  WPR2_HI       = 0x%08X\n", Wpr2Hi);
  LogPrint (L"  WPR2 Enabled  = %a\n", IsWpr2Enabled () ? "YES" : "NO");
  LogPrint (L"  GSP CPUCTL    = 0x%08X (Halted: %a)\n", GspCpuctl,
         (GspCpuctl & FALCON_CPUCTL_HALTED) ? "YES" : "NO");
}

//=============================================================================
// GPU Discovery
//=============================================================================

STATIC
EFI_STATUS
FindNvidiaGpu (
  VOID
  )
{
  EFI_STATUS          Status;
  UINTN               HandleCount;
  EFI_HANDLE          *HandleBuffer;
  UINTN               Index;
  UINT16              VendorId;
  UINT16              DeviceId;
  EFI_PCI_IO_PROTOCOL *PciIo;
  UINT64              BarAddress;
  UINT64              BarSize;
  VOID                *BarMapping;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiPciIoProtocolGuid,
                  NULL,
                  &HandleCount,
                  &HandleBuffer
                  );

  if (EFI_ERROR (Status)) {
    Print (L"NVDAAL: Failed to locate PCI handles\n");
    return Status;
  }

  for (Index = 0; Index < HandleCount; Index++) {
    Status = gBS->HandleProtocol (
                    HandleBuffer[Index],
                    &gEfiPciIoProtocolGuid,
                    (VOID **)&PciIo
                    );

    if (EFI_ERROR (Status)) {
      continue;
    }

    PciIo->Pci.Read (PciIo, EfiPciIoWidthUint16, 0x00, 1, &VendorId);
    PciIo->Pci.Read (PciIo, EfiPciIoWidthUint16, 0x02, 1, &DeviceId);

    if (VendorId == NVIDIA_VENDOR_ID) {
      // Check if it's an Ada Lovelace GPU
      if (DeviceId == NVIDIA_RTX4090_ID1 || DeviceId == NVIDIA_RTX4090_ID2 ||
          DeviceId == NVIDIA_RTX4080_ID1 || DeviceId == NVIDIA_RTX4070TI_ID1) {

        Print (L"NVDAAL: Found NVIDIA GPU (0x%04X:0x%04X)\n", VendorId, DeviceId);

        // Get BAR0 info
        Status = PciIo->GetBarAttributes (PciIo, 0, NULL, (VOID **)&BarMapping);
        if (!EFI_ERROR (Status)) {
          FreePool (BarMapping);
        }

        // FIX: Properly handle 32-bit and 64-bit BARs
        UINT32 BarLo, BarHi;
        PciIo->Pci.Read (PciIo, EfiPciIoWidthUint32, 0x10, 1, &BarLo);

        // Check if it's a memory BAR (bit 0 must be 0)
        if (BarLo & 0x1) {
          Print (L"NVDAAL: BAR0 is I/O BAR, not memory - skipping\n");
          continue;
        }

        // Check if it's a 64-bit BAR (bits 2:1 = 10b means 64-bit)
        if ((BarLo & 0x6) == 0x4) {
          // 64-bit BAR: read high 32 bits
          PciIo->Pci.Read (PciIo, EfiPciIoWidthUint32, 0x14, 1, &BarHi);
          BarAddress = ((UINT64)BarHi << 32) | (BarLo & ~0xFULL);
          Print (L"NVDAAL: 64-bit BAR0 detected\n");
        } else {
          // 32-bit BAR
          BarAddress = BarLo & ~0xFULL;
          Print (L"NVDAAL: 32-bit BAR0 detected\n");
        }

        // Validate BAR address
        if (BarAddress == 0) {
          Print (L"NVDAAL: BAR0 address is NULL - GPU may not be initialized\n");
          continue;
        }

        // Get BAR0 size
        BarSize = 16 * 1024 * 1024;  // Assume 16MB for MMIO

        Print (L"NVDAAL: BAR0 @ 0x%lX (size: %u MB)\n", BarAddress, BarSize / (1024 * 1024));

        mMmioBase = (UINT32 *)(UINTN)BarAddress;
        mPciIo = PciIo;

        FreePool (HandleBuffer);
        return EFI_SUCCESS;
      }
    }
  }

  FreePool (HandleBuffer);
  Print (L"NVDAAL: No compatible NVIDIA GPU found\n");
  return EFI_NOT_FOUND;
}

//=============================================================================
// VBIOS Parsing
//=============================================================================

// Enable ROM access on NVIDIA GPUs
STATIC
VOID
EnableRomAccess (
  VOID
  )
{
  UINT32 Val;

  // NV_PBUS_PCI_NV_20 - ROM Shadow enable
  #define NV_PBUS_PCI_NV_20  0x00001850

  // Enable ROM shadow/access
  Val = ReadReg (NV_PBUS_PCI_NV_20);
  LogPrint (L"NVDAAL: NV_PBUS_PCI_NV_20 before = 0x%08X\n", Val);
  WriteReg (NV_PBUS_PCI_NV_20, Val | 1);  // Enable ROM access

  // Also try enabling via PCI config if needed
  // Some GPUs need ROM BAR to be enabled
}

STATIC
EFI_STATUS
ReadVbiosFromGpu (
  OUT UINT8   **VbiosData,
  OUT UINTN   *VbiosSize
  )
{
  UINT8   *Vbios;
  UINTN   MaxSize = 0x200000;  // 2MB
  UINTN   i;
  UINT32  FirstDword;

  LogPrint (L"NVDAAL: Enabling ROM access...\n");
  EnableRomAccess ();

  Vbios = AllocateZeroPool (MaxSize);
  if (Vbios == NULL) {
    LogPrint (L"NVDAAL: Failed to allocate VBIOS buffer\n");
    return EFI_OUT_OF_RESOURCES;
  }

  LogPrint (L"NVDAAL: Reading VBIOS from GPU ROM @ 0x%X...\n", VBIOS_ROM_OFFSET);

  // Read first dword to check if ROM is accessible
  FirstDword = ReadReg (VBIOS_ROM_OFFSET);
  LogPrint (L"NVDAAL: First DWORD @ ROM = 0x%08X\n", FirstDword);

  if (FirstDword == 0x00000000 || FirstDword == 0xFFFFFFFF) {
    LogPrint (L"NVDAAL: ROM appears inaccessible (returned 0x%08X)\n", FirstDword);
    LogPrint (L"NVDAAL: Trying alternative ROM offset...\n");

    // Try NV_PROM offset
    #define NV_PROM  0x00300000
    FirstDword = ReadReg (NV_PROM);
    LogPrint (L"NVDAAL: NV_PROM first DWORD = 0x%08X\n", FirstDword);
  }

  for (i = 0; i < MaxSize; i += 4) {
    UINT32 Val = ReadReg (VBIOS_ROM_OFFSET + i);
    Vbios[i + 0] = (UINT8)(Val >> 0);
    Vbios[i + 1] = (UINT8)(Val >> 8);
    Vbios[i + 2] = (UINT8)(Val >> 16);
    Vbios[i + 3] = (UINT8)(Val >> 24);
  }

  // Check for NVGI or 55AA header
  if (Vbios[0] == 'N' && Vbios[1] == 'V' && Vbios[2] == 'G' && Vbios[3] == 'I') {
    LogPrint (L"NVDAAL: VBIOS has NVGI header\n");
    LogPrint (L"NVDAAL: NVGI bytes: %02X %02X %02X %02X %02X %02X %02X %02X\n",
              Vbios[0], Vbios[1], Vbios[2], Vbios[3],
              Vbios[4], Vbios[5], Vbios[6], Vbios[7]);
  } else if (Vbios[0] == 0x55 && Vbios[1] == 0xAA) {
    LogPrint (L"NVDAAL: VBIOS has standard ROM header\n");
  } else {
    LogPrint (L"NVDAAL: Unexpected VBIOS header: 0x%02X%02X%02X%02X\n",
              Vbios[0], Vbios[1], Vbios[2], Vbios[3]);
    LogPrint (L"NVDAAL: First 32 bytes:\n");
    for (i = 0; i < 32; i += 8) {
      LogPrint (L"  %02X %02X %02X %02X %02X %02X %02X %02X\n",
                Vbios[i+0], Vbios[i+1], Vbios[i+2], Vbios[i+3],
                Vbios[i+4], Vbios[i+5], Vbios[i+6], Vbios[i+7]);
    }
    // Still continue - might find valid data later
  }

  *VbiosData = Vbios;
  *VbiosSize = MaxSize;

  // Save VBIOS to file for offline analysis
  {
    EFI_FILE_PROTOCOL *VbiosFile = NULL;
    EFI_STATUS SaveStatus;

    if (mLogRoot != NULL) {
      SaveStatus = mLogRoot->Open (
        mLogRoot,
        &VbiosFile,
        L"NVDAAL_VBIOS.bin",
        EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
        0
        );

      if (!EFI_ERROR (SaveStatus) && VbiosFile != NULL) {
        UINTN WriteSize = MaxSize;
        VbiosFile->Write (VbiosFile, &WriteSize, Vbios);
        VbiosFile->Close (VbiosFile);
        LogPrint (L"NVDAAL: VBIOS saved to NVDAAL_VBIOS.bin (%u bytes)\n", MaxSize);
      }
    }
  }

  return EFI_SUCCESS;
}

// Sentinel value for "not found" since 0 is a valid ROM offset
#define ROM_NOT_FOUND  0xFFFFFFFF

STATIC
UINT32
FindRomImageBase (
  IN CONST UINT8  *Data,
  IN UINTN        Size,
  IN UINT32       SearchStart
  )
{
  UINT32  Offset;
  UINT32  i;

  // First check offset 0 - GPU ROM often starts directly with 55AA
  if (SearchStart == 0 && Size >= 2) {
    if (Data[0] == 0x55 && Data[1] == 0xAA) {
      LogPrint (L"NVDAAL: Found ROM signature at offset 0x0\n");
      return 0;
    }
  }

  // Try known NVGI ROM offsets
  UINT32 KnownOffsets[] = {0x9400, 0x19000, 0xFC00};

  for (i = 0; i < sizeof(KnownOffsets)/sizeof(KnownOffsets[0]); i++) {
    Offset = KnownOffsets[i];
    if (Offset >= SearchStart && Offset < Size - 2) {
      if (Data[Offset] == 0x55 && Data[Offset + 1] == 0xAA) {
        LogPrint (L"NVDAAL: Found ROM signature at known offset 0x%X\n", Offset);
        return Offset;
      }
    }
  }

  // Fall back to scanning
  LogPrint (L"NVDAAL: Scanning for ROM signature from 0x%X...\n", SearchStart);
  for (Offset = SearchStart; Offset < Size - 2; Offset += 512) {
    if (Data[Offset] == 0x55 && Data[Offset + 1] == 0xAA) {
      LogPrint (L"NVDAAL: Found ROM signature at offset 0x%X\n", Offset);
      return Offset;
    }
  }

  return ROM_NOT_FOUND;
}

STATIC
EFI_STATUS
ParseVbios (
  IN CONST UINT8  *Data,
  IN UINTN        Size
  )
{
  UINT32              Offset;
  UINT32              ImageBase;
  UINT32              BitOffset;
  UINT32              FalconDataOffset;
  UINT32              PmuTableOffset;
  BIT_HEADER          *Bit;
  BIT_TOKEN           *Token;
  BIT_FALCON_DATA     *FalconData;
  PMU_LOOKUP_TABLE_HEADER *PmuHdr;
  PMU_LOOKUP_ENTRY    *PmuEntry;
  FALCON_UCODE_DESC_V3 *UcodeDesc;
  UINT32              i;
  UINT8               BitPattern[] = {0xFF, 0xB8, 'B', 'I', 'T', 0x00};

  LogPrint (L"NVDAAL: Parsing VBIOS (%u bytes)...\n", Size);

  ZeroMem (&mFwsecInfo, sizeof(mFwsecInfo));

  // Step 1: Find first ROM image (may have NVGI header before it)
  LogPrint (L"NVDAAL: Step 1 - Finding ROM image base...\n");
  ImageBase = FindRomImageBase (Data, Size, 0);

  if (ImageBase == ROM_NOT_FOUND) {
    LogPrint (L"NVDAAL: No ROM image found in VBIOS!\n");
    LogPrint (L"NVDAAL: Dumping first 64 bytes of VBIOS data:\n");
    for (UINT32 dbg = 0; dbg < 64 && dbg < Size; dbg += 16) {
      LogPrint (L"  %04X: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                dbg,
                Data[dbg+0], Data[dbg+1], Data[dbg+2], Data[dbg+3],
                Data[dbg+4], Data[dbg+5], Data[dbg+6], Data[dbg+7],
                Data[dbg+8], Data[dbg+9], Data[dbg+10], Data[dbg+11],
                Data[dbg+12], Data[dbg+13], Data[dbg+14], Data[dbg+15]);
    }
    return EFI_NOT_FOUND;
  }

  LogPrint (L"NVDAAL: ROM image base @ 0x%X\n", ImageBase);

  // Step 2: Find BIT header
  LogPrint (L"NVDAAL: Step 2 - Searching for BIT header...\n");
  BitOffset = 0;
  for (Offset = ImageBase; Offset < Size - sizeof(BitPattern); Offset++) {
    if (CompareMem (Data + Offset, BitPattern, sizeof(BitPattern)) == 0) {
      BitOffset = Offset;
      break;
    }
  }

  if (BitOffset == 0) {
    LogPrint (L"NVDAAL: BIT header not found in range 0x%X-0x%lX\n", ImageBase, Size);
    // Dump some data to help debug
    UINT32 DumpStart = ImageBase + 0x170;  // BIT is usually around offset 0x1B0
    if (DumpStart + 32 < Size) {
      LogPrint (L"NVDAAL: Data at 0x%X:\n", DumpStart);
      LogPrint (L"  %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                Data[DumpStart+0], Data[DumpStart+1], Data[DumpStart+2], Data[DumpStart+3],
                Data[DumpStart+4], Data[DumpStart+5], Data[DumpStart+6], Data[DumpStart+7],
                Data[DumpStart+8], Data[DumpStart+9], Data[DumpStart+10], Data[DumpStart+11],
                Data[DumpStart+12], Data[DumpStart+13], Data[DumpStart+14], Data[DumpStart+15]);
    }
    return EFI_NOT_FOUND;
  }

  LogPrint (L"NVDAAL: BIT header @ 0x%X\n", BitOffset);

  Bit = (BIT_HEADER *)(Data + BitOffset);
  LogPrint (L"NVDAAL: BIT: headerSize=%u, tokenSize=%u, tokenCount=%u\n",
            Bit->HeaderSize, Bit->TokenSize, Bit->TokenCount);

  // Step 3: Find FALCON_DATA token (0x70)
  LogPrint (L"NVDAAL: Step 3 - Finding FALCON_DATA token...\n");
  FalconDataOffset = 0;
  Offset = BitOffset + Bit->HeaderSize;

  for (i = 0; i < Bit->TokenCount; i++) {
    Token = (BIT_TOKEN *)(Data + Offset);
    LogPrint (L"NVDAAL: Token %u: id=0x%02X dataOff=0x%X\n",
              i, Token->Id, Token->DataOffset);

    if (Token->Id == BIT_TOKEN_FALCON_DATA) {
      FalconDataOffset = ImageBase + Token->DataOffset;
      LogPrint (L"NVDAAL: Found FALCON_DATA @ 0x%X (data @ 0x%X)\n",
                Offset, FalconDataOffset);
      break;
    }

    Offset += Bit->TokenSize;
  }

  if (FalconDataOffset == 0 || FalconDataOffset >= Size) {
    LogPrint (L"NVDAAL: Falcon Data token not found\n");
    return EFI_NOT_FOUND;
  }

  // Step 4: Read PMU Lookup Table offset from Falcon Data
  LogPrint (L"NVDAAL: Step 4 - Reading PMU Lookup Table...\n");
  FalconData = (BIT_FALCON_DATA *)(Data + FalconDataOffset);

  // Debug: dump first bytes of FalconData
  LogPrint (L"NVDAAL: FalconData @ 0x%X bytes:\n", FalconDataOffset);
  LogPrint (L"  %02X %02X %02X %02X %02X %02X %02X %02X\n",
            Data[FalconDataOffset+0], Data[FalconDataOffset+1],
            Data[FalconDataOffset+2], Data[FalconDataOffset+3],
            Data[FalconDataOffset+4], Data[FalconDataOffset+5],
            Data[FalconDataOffset+6], Data[FalconDataOffset+7]);

  PmuTableOffset = ImageBase + FalconData->UcodeTableOffset;

  LogPrint (L"NVDAAL: PMU Lookup Table @ 0x%X (raw UcodeTableOffset=0x%X)\n",
            PmuTableOffset, FalconData->UcodeTableOffset);

  if (PmuTableOffset >= Size - sizeof(PMU_LOOKUP_TABLE_HEADER)) {
    LogPrint (L"NVDAAL: Invalid PMU table offset (0x%X >= 0x%lX)\n",
              PmuTableOffset, Size - sizeof(PMU_LOOKUP_TABLE_HEADER));
    return EFI_NOT_FOUND;
  }

  // Step 5: Parse PMU Lookup Table
  LogPrint (L"NVDAAL: Step 5 - Parsing PMU entries...\n");
  PmuHdr = (PMU_LOOKUP_TABLE_HEADER *)(Data + PmuTableOffset);
  LogPrint (L"NVDAAL: PMU Table: version=%u, header=%u, entrySize=%u, entries=%u\n",
            PmuHdr->Version, PmuHdr->HeaderSize, PmuHdr->EntrySize, PmuHdr->EntryCount);

  Offset = PmuTableOffset + PmuHdr->HeaderSize;

  for (i = 0; i < PmuHdr->EntryCount; i++) {
    if (Offset + PmuHdr->EntrySize > Size) {
      break;
    }

    PmuEntry = (PMU_LOOKUP_ENTRY *)(Data + Offset);

    LogPrint (L"NVDAAL: PMU Entry %u: appId=0x%02X, targetId=0x%02X, dataOff=0x%X\n",
              i, PmuEntry->AppId, PmuEntry->TargetId, PmuEntry->DataOffset);

    // Look for FWSEC (0x85) or FRTS (0x01)
    if (PmuEntry->AppId == FWSEC_APP_ID_FWSEC || PmuEntry->AppId == FWSEC_APP_ID_FRTS) {
      UINT32 UcodeOffset = PmuEntry->DataOffset;

      // Data offset is relative to image base
      if (UcodeOffset < 0x100000) {
        UcodeOffset += ImageBase;
      }

      if (UcodeOffset + sizeof(FALCON_UCODE_DESC_V3) > Size) {
        Print (L"NVDAAL: Invalid ucode descriptor offset\n");
        continue;
      }

      UcodeDesc = (FALCON_UCODE_DESC_V3 *)(Data + UcodeOffset);

      Print (L"NVDAAL: Ucode Desc @ 0x%X:\n", UcodeOffset);
      Print (L"  IMEM: off=0x%X, size=%u, secure=%u\n",
             UcodeDesc->ImemOffset, UcodeDesc->ImemLoadSize, UcodeDesc->ImemSecureSize);
      Print (L"  DMEM: off=0x%X, size=%u\n",
             UcodeDesc->DmemOffset, UcodeDesc->DmemSize);
      Print (L"  BootVec: 0x%X\n", UcodeDesc->BootVec);

      // Store FWSEC info with bounds validation
      mFwsecInfo.ImemOffset = UcodeOffset + UcodeDesc->ImemOffset;
      mFwsecInfo.ImemSize = UcodeDesc->ImemLoadSize;
      mFwsecInfo.ImemSecSize = UcodeDesc->ImemSecureSize;
      mFwsecInfo.DmemOffset = UcodeOffset + UcodeDesc->DmemOffset;
      mFwsecInfo.DmemSize = UcodeDesc->DmemSize;
      mFwsecInfo.BootVec = UcodeDesc->BootVec;

      // Capture signature info for BROM interface
      mFwsecInfo.SigOffset = UcodeOffset + UcodeDesc->SigOffset;
      mFwsecInfo.SigSize = UcodeDesc->SigSize;
      mFwsecInfo.StoredSize = UcodeDesc->StoredSize;

      Print (L"  Signature: off=0x%X, size=%u\n",
             UcodeDesc->SigOffset, UcodeDesc->SigSize);
      Print (L"  StoredSize: %u\n", UcodeDesc->StoredSize);

      // FIX: Validate all offsets are within VBIOS bounds
      if (mFwsecInfo.ImemOffset + mFwsecInfo.ImemSize > Size) {
        Print (L"NVDAAL: IMEM offset out of bounds (0x%X + 0x%X > 0x%X)\n",
               mFwsecInfo.ImemOffset, mFwsecInfo.ImemSize, Size);
        continue;
      }

      if (mFwsecInfo.DmemOffset + mFwsecInfo.DmemSize > Size) {
        Print (L"NVDAAL: DMEM offset out of bounds (0x%X + 0x%X > 0x%X)\n",
               mFwsecInfo.DmemOffset, mFwsecInfo.DmemSize, Size);
        continue;
      }

      // Validate sizes are reasonable
      if (mFwsecInfo.ImemSize == 0 || mFwsecInfo.ImemSize > 0x100000) {
        Print (L"NVDAAL: Invalid IMEM size: 0x%X\n", mFwsecInfo.ImemSize);
        continue;
      }

      if (mFwsecInfo.DmemSize == 0 || mFwsecInfo.DmemSize > 0x100000) {
        Print (L"NVDAAL: Invalid DMEM size: 0x%X\n", mFwsecInfo.DmemSize);
        continue;
      }

      // Step 6: Find DMEMMAPPER in DMEM
      if (mFwsecInfo.DmemOffset + mFwsecInfo.DmemSize <= Size && mFwsecInfo.DmemSize > 4) {
        CONST UINT8 *Dmem = Data + mFwsecInfo.DmemOffset;
        UINT32 j;

        for (j = 0; j < mFwsecInfo.DmemSize - 4; j += 4) {
          if (*(UINT32 *)(Dmem + j) == DMEMMAPPER_SIGNATURE) {
            // FIX: Validate DMEMMAPPER offset has room for header
            if (j + sizeof(DMEMMAPPER_HEADER) <= mFwsecInfo.DmemSize) {
              mFwsecInfo.DmemMapperOffset = j;
              Print (L"NVDAAL: Found DMEMMAPPER @ DMEM+0x%X\n", j);
            } else {
              Print (L"NVDAAL: DMEMMAPPER found but truncated at 0x%X\n", j);
            }
            break;
          }
        }
      }

      mFwsecInfo.Valid = TRUE;
      Print (L"NVDAAL: FWSEC info extracted successfully!\n");
      break;
    }

    Offset += PmuHdr->EntrySize;
  }

  if (!mFwsecInfo.Valid) {
    Print (L"NVDAAL: Could not extract FWSEC info from PMU table\n");
    return EFI_NOT_FOUND;
  }

  return EFI_SUCCESS;
}

//=============================================================================
// BROM Interface - Proper Heavy-Secure Mode Execution
// Based on Linux nova-core falcon.rs and boot.rs implementation
//=============================================================================

STATIC DMA_BUFFER mDmaBuffer = {NULL, 0, 0, FALSE};

STATIC
EFI_STATUS
AllocateDmaBuffer (
  IN UINTN  Size
  )
{
  EFI_STATUS Status;
  EFI_PHYSICAL_ADDRESS PhysAddr;
  UINTN Pages;

  // Free existing buffer if any
  if (mDmaBuffer.Valid && mDmaBuffer.VirtualAddr != NULL) {
    gBS->FreePages (mDmaBuffer.PhysAddr, EFI_SIZE_TO_PAGES (mDmaBuffer.Size));
    mDmaBuffer.Valid = FALSE;
  }

  // Allocate pages below 4GB for DMA accessibility
  Pages = EFI_SIZE_TO_PAGES (Size);
  PhysAddr = 0xFFFFFFFF;  // Request memory below 4GB

  Status = gBS->AllocatePages (
                  AllocateMaxAddress,
                  EfiBootServicesData,
                  Pages,
                  &PhysAddr
                  );

  if (EFI_ERROR (Status)) {
    Print (L"NVDAAL: Failed to allocate DMA buffer (%u pages): %r\n", Pages, Status);
    return Status;
  }

  mDmaBuffer.PhysAddr = PhysAddr;
  mDmaBuffer.VirtualAddr = (UINT8 *)(UINTN)PhysAddr;
  mDmaBuffer.Size = Size;
  mDmaBuffer.Valid = TRUE;

  // Zero the buffer
  ZeroMem (mDmaBuffer.VirtualAddr, Size);

  Print (L"NVDAAL: DMA buffer allocated at phys 0x%lX (size %u)\n", PhysAddr, Size);

  return EFI_SUCCESS;
}

STATIC
VOID
FreeDmaBuffer (
  VOID
  )
{
  if (mDmaBuffer.Valid && mDmaBuffer.VirtualAddr != NULL) {
    gBS->FreePages (mDmaBuffer.PhysAddr, EFI_SIZE_TO_PAGES (mDmaBuffer.Size));
    mDmaBuffer.Valid = FALSE;
    mDmaBuffer.VirtualAddr = NULL;
    mDmaBuffer.PhysAddr = 0;
    mDmaBuffer.Size = 0;
  }
}

// Load firmware to Falcon IMEM using PIO (direct register writes)
// This is an alternative to DMA that doesn't require external memory access
STATIC
EFI_STATUS
LoadImemViaPio (
  IN UINT32        FalconBase,
  IN CONST UINT32  *Data,
  IN UINT32        SizeBytes,
  IN UINT32        DstOffset
  )
{
  UINT32  NumWords = (SizeBytes + 3) / 4;
  UINT32  i;
  UINT32  ImemcVal;

  LogPrint (L"NVDAAL: PIO loading %u bytes to IMEM @ 0x%X\n", SizeBytes, DstOffset);

  // Set up IMEMC for auto-increment writes
  // Format: [31:24]=port, [23]=secure, [22]=auto-inc, [7:2]=offset/4
  ImemcVal = FALCON_IMEMC_AINCW | (DstOffset >> 2);

  WriteReg (FalconBase + FALCON_IMEMC(0), ImemcVal);

  // Verify IMEMC was written
  {
    UINT32 Readback = ReadReg (FalconBase + FALCON_IMEMC(0));
    LogPrint (L"NVDAAL: IMEMC[0] = 0x%08X (wrote 0x%08X)\n", Readback, ImemcVal);
  }

  // Write data words
  for (i = 0; i < NumWords; i++) {
    WriteReg (FalconBase + FALCON_IMEMD(0), Data[i]);
  }

  LogPrint (L"NVDAAL: PIO wrote %u words to IMEM\n", NumWords);
  return EFI_SUCCESS;
}

// Load firmware to Falcon DMEM using PIO (direct register writes)
STATIC
EFI_STATUS
LoadDmemViaPio (
  IN UINT32        FalconBase,
  IN CONST UINT32  *Data,
  IN UINT32        SizeBytes,
  IN UINT32        DstOffset
  )
{
  UINT32  NumWords = (SizeBytes + 3) / 4;
  UINT32  i;
  UINT32  DmemcVal;

  LogPrint (L"NVDAAL: PIO loading %u bytes to DMEM @ 0x%X\n", SizeBytes, DstOffset);

  // Set up DMEMC for auto-increment writes
  DmemcVal = FALCON_DMEMC_AINCW | (DstOffset >> 2);

  WriteReg (FalconBase + FALCON_DMEMC(0), DmemcVal);

  // Verify
  {
    UINT32 Readback = ReadReg (FalconBase + FALCON_DMEMC(0));
    LogPrint (L"NVDAAL: DMEMC[0] = 0x%08X (wrote 0x%08X)\n", Readback, DmemcVal);
  }

  // Write data words
  for (i = 0; i < NumWords; i++) {
    WriteReg (FalconBase + FALCON_DMEMD(0), Data[i]);
  }

  LogPrint (L"NVDAAL: PIO wrote %u words to DMEM\n", NumWords);
  return EFI_SUCCESS;
}

// Perform a single 256-byte DMA transfer to Falcon memory
STATIC
EFI_STATUS
DmaTransfer256 (
  IN UINT32  FalconBase,
  IN UINT32  FalconMemOffset,  // Offset in Falcon IMEM/DMEM
  IN UINT32  SrcOffset,        // Offset from DMA base
  IN BOOLEAN ToImem,           // TRUE = IMEM, FALSE = DMEM
  IN BOOLEAN Verbose           // Log details
  )
{
  UINT32  Cmd;
  UINT32  CmdStatus;
  UINTN   Timeout;

  // Set falcon memory offset
  WriteReg (FalconBase + FALCON_DMATRFMOFFS, FalconMemOffset);

  // Set source offset (relative to DMA base)
  WriteReg (FalconBase + FALCON_DMATRFFBOFFS, SrcOffset);

  // Build command: 256-byte transfer, to IMEM or DMEM
  // Bits: [1]=idle(RO), [3:2]=sec, [4]=imem, [10:8]=size
  // Note: NOT setting sec=1 (bit 2) - security fuses may not be configured
  // This allows DMA to work in non-secure mode
  Cmd = DMATRFCMD_SIZE_256B;  // Size = 6 (256 bytes)
  if (ToImem) {
    Cmd |= DMATRFCMD_IMEM;
  }

  if (Verbose) {
    LogPrint (L"NVDAAL: DMA cmd=0x%X, moffs=0x%X, fboffs=0x%X\n",
      Cmd, FalconMemOffset, SrcOffset);
  }

  // Check status before issuing command
  CmdStatus = ReadReg (FalconBase + FALCON_DMATRFCMD);
  if (Verbose) {
    LogPrint (L"NVDAAL: DMATRFCMD before write: 0x%08X\n", CmdStatus);
  }

  // Issue the transfer command
  WriteReg (FalconBase + FALCON_DMATRFCMD, Cmd);

  // Read back immediately
  CmdStatus = ReadReg (FalconBase + FALCON_DMATRFCMD);
  if (Verbose) {
    LogPrint (L"NVDAAL: DMATRFCMD after write: 0x%08X\n", CmdStatus);
  }

  // Wait for transfer to complete (idle bit set)
  for (Timeout = 0; Timeout < 2000; Timeout++) {
    gBS->Stall (1);  // 1us
    CmdStatus = ReadReg (FalconBase + FALCON_DMATRFCMD);
    if (CmdStatus & DMATRFCMD_IDLE) {
      if (Verbose) {
        LogPrint (L"NVDAAL: DMA complete after %u us, status=0x%08X\n", Timeout, CmdStatus);
      }
      return EFI_SUCCESS;
    }
  }

  // Timeout - log detailed status
  LogPrint (L"NVDAAL: DMA transfer timeout at offset 0x%X\n", FalconMemOffset);
  LogPrint (L"NVDAAL: Final DMATRFCMD=0x%08X, DMACTL=0x%08X\n",
    CmdStatus, ReadReg (FalconBase + FALCON_DMACTL));
  LogPrint (L"NVDAAL: TRANSCFG=0x%08X, FBIF_CTL=0x%08X\n",
    ReadReg (FalconBase + FALCON_FBIF_TRANSCFG),
    ReadReg (FalconBase + FALCON_FBIF_CTL));

  return EFI_TIMEOUT;
}

// Execute scrubber firmware via PIO (Direct Register Access)
// This function properly handles the scrubber firmware format with separate IMEM/DMEM sections
STATIC
EFI_STATUS
ExecuteScrubberViaPio (
  IN UINT32       FalconBase,
  IN CONST UINT8  *ScrubberData,
  IN UINTN        ScrubberSize
  )
{
  EFI_STATUS  Status;
  SCRUBBER_HEADER  *Hdr;
  UINT32      Hwcfg2;
  UINT32      Cpuctl;
  UINT32      Mailbox0;
  UINTN       Timeout;

  LogPrint (L"\n=== Scrubber PIO Load (direct register method) ===\n");

  if (ScrubberSize < sizeof(SCRUBBER_HEADER)) {
    LogPrint (L"NVDAAL: Scrubber data too small\n");
    return EFI_INVALID_PARAMETER;
  }

  Hdr = (SCRUBBER_HEADER *)ScrubberData;

  // Validate header
  if (Hdr->VendorId != 0x10DE) {
    LogPrint (L"NVDAAL: Invalid scrubber header (VendorId=0x%04X)\n", Hdr->VendorId);
    return EFI_INVALID_PARAMETER;
  }

  // Validate IMEM/DMEM offsets are within bounds
  if (Hdr->ImemOffset + Hdr->ImemSize > ScrubberSize) {
    LogPrint (L"NVDAAL: IMEM section out of bounds\n");
    return EFI_INVALID_PARAMETER;
  }
  if (Hdr->DmemOffset + Hdr->DmemSize > ScrubberSize) {
    LogPrint (L"NVDAAL: DMEM section out of bounds\n");
    return EFI_INVALID_PARAMETER;
  }

  LogPrint (L"NVDAAL: Scrubber IMEM @ 0x%X, size 0x%X (%u bytes)\n",
    Hdr->ImemOffset, Hdr->ImemSize, Hdr->ImemSize);
  LogPrint (L"NVDAAL: Scrubber DMEM @ 0x%X, size 0x%X (%u bytes)\n",
    Hdr->DmemOffset, Hdr->DmemSize, Hdr->DmemSize);

  // Step 1: Read hardware config
  Hwcfg2 = ReadReg (FalconBase + FALCON_HWCFG2);
  LogPrint (L"NVDAAL: HWCFG2 = 0x%08X\n", Hwcfg2);

  // Step 2: Reset Falcon engine
  LogPrint (L"NVDAAL: Resetting Falcon engine...\n");
  WriteReg (FalconBase + FALCON_ENGINE, 1);
  gBS->Stall (100);
  WriteReg (FalconBase + FALCON_ENGINE, 0);
  gBS->Stall (100);

  // Step 3: Wait briefly for any internal initialization
  gBS->Stall (1000);  // 1ms

  // Step 4: Select Falcon core (for dual RISCV/Falcon)
  {
    UINT32 BcrCtrl = ReadReg (FalconBase + FALCON_RISCV_BCR_CTRL);
    LogPrint (L"NVDAAL: BCR_CTRL before: 0x%08X\n", BcrCtrl);

    // Write 1 to select Falcon core
    WriteReg (FalconBase + FALCON_RISCV_BCR_CTRL, 1);

    for (Timeout = 0; Timeout < 1000; Timeout++) {
      BcrCtrl = ReadReg (FalconBase + FALCON_RISCV_BCR_CTRL);
      if ((BcrCtrl & 0x3) == 1) {
        break;
      }
      gBS->Stall (1);
    }
    LogPrint (L"NVDAAL: BCR_CTRL after: 0x%08X\n", BcrCtrl);
  }

  // Step 5: Load DMEM via PIO (must be done before IMEM for some firmwares)
  if (Hdr->DmemSize > 0) {
    LogPrint (L"NVDAAL: Loading DMEM section via PIO...\n");
    Status = LoadDmemViaPio (
      FalconBase,
      (CONST UINT32 *)(ScrubberData + Hdr->DmemOffset),
      Hdr->DmemSize,
      0  // Load at DMEM offset 0
    );
    if (EFI_ERROR (Status)) {
      LogPrint (L"NVDAAL: DMEM PIO load failed\n");
      return Status;
    }
    LogPrint (L"NVDAAL: DMEM loaded successfully\n");
  }

  // Step 6: Load IMEM via PIO
  LogPrint (L"NVDAAL: Loading IMEM section via PIO...\n");
  Status = LoadImemViaPio (
    FalconBase,
    (CONST UINT32 *)(ScrubberData + Hdr->ImemOffset),
    Hdr->ImemSize,
    0  // Load at IMEM offset 0
  );
  if (EFI_ERROR (Status)) {
    LogPrint (L"NVDAAL: IMEM PIO load failed\n");
    return Status;
  }
  LogPrint (L"NVDAAL: IMEM loaded successfully\n");

  // Step 7: Set boot vector to 0
  LogPrint (L"NVDAAL: Setting boot vector to 0...\n");
  WriteReg (FalconBase + FALCON_BOOTVEC, 0);

  // Step 8: Start Falcon execution
  LogPrint (L"NVDAAL: Starting Falcon execution...\n");
  WriteReg (FalconBase + FALCON_CPUCTL, 2);  // Start CPU

  // Step 9: Wait for completion
  LogPrint (L"NVDAAL: Waiting for scrubber to complete...\n");
  for (Timeout = 0; Timeout < 100000; Timeout++) {  // 100ms max
    gBS->Stall (1);

    Cpuctl = ReadReg (FalconBase + FALCON_CPUCTL);
    if (Cpuctl & 0x10) {  // Halted bit
      Mailbox0 = ReadReg (FalconBase + FALCON_MAILBOX0);
      LogPrint (L"NVDAAL: Falcon halted after %u us, CPUCTL=0x%08X, MBOX0=0x%08X\n",
        Timeout, Cpuctl, Mailbox0);
      return EFI_SUCCESS;
    }
  }

  Cpuctl = ReadReg (FalconBase + FALCON_CPUCTL);
  Mailbox0 = ReadReg (FalconBase + FALCON_MAILBOX0);
  LogPrint (L"NVDAAL: Scrubber timeout, CPUCTL=0x%08X, MBOX0=0x%08X\n", Cpuctl, Mailbox0);

  // Return success anyway - WPR2 will be checked after
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
ExecuteFwsecViaBrom (
  IN UINT32       FalconBase,
  IN CONST UINT8  *FirmwareData,  // Complete firmware blob (sigs + IMEM + DMEM)
  IN UINTN        FirmwareSize,
  IN UINT32       BootVec
  )
{
  EFI_STATUS  Status;
  UINT32      Cpuctl;
  UINT32      Mailbox0;
  UINTN       Timeout;
  UINT32      DmaBaseLo;
  UINT32      DmaBaseHi;
  UINT32      Pos;
  UINT32      Hwcfg2;

  LogPrint (L"\n=== Falcon DMA Load (nova-core method) ===\n");
  LogPrint (L"NVDAAL: Firmware size: %u bytes\n", FirmwareSize);
  LogPrint (L"NVDAAL: Boot vector: 0x%X\n", BootVec);

  // Read hardware config
  Hwcfg2 = ReadReg (FalconBase + FALCON_HWCFG2);
  LogPrint (L"NVDAAL: HWCFG2 = 0x%08X (RISCV=%d, MemScrub=%d)\n",
    Hwcfg2, (Hwcfg2 >> 10) & 1, (Hwcfg2 >> 12) & 1);

  // Step 1: Allocate DMA buffer for firmware
  Status = AllocateDmaBuffer (FirmwareSize);
  if (EFI_ERROR (Status)) {
    LogPrint (L"NVDAAL: DMA allocation failed: %r\n", Status);
    return Status;
  }

  // Step 2: Copy firmware to DMA buffer
  CopyMem (mDmaBuffer.VirtualAddr, FirmwareData, FirmwareSize);
  LogPrint (L"NVDAAL: Firmware copied to DMA buffer @ phys 0x%lX\n", mDmaBuffer.PhysAddr);

  // Step 3: Flush memory
  MemoryFence ();

  // Step 4: Reset the Falcon engine
  LogPrint (L"NVDAAL: Resetting Falcon engine...\n");

  // First read HWCFG2 to check if RISCV is present
  Hwcfg2 = ReadReg (FalconBase + FALCON_HWCFG2);
  LogPrint (L"NVDAAL: HWCFG2 before reset: 0x%08X\n", Hwcfg2);

  // Assert reset
  WriteReg (FalconBase + FALCON_ENGINE, 1);
  gBS->Stall (100);  // 100us for reset to take effect

  // Deassert reset
  WriteReg (FalconBase + FALCON_ENGINE, 0);
  gBS->Stall (100);  // 100us for reset to complete

  // Wait for memory scrubbing to complete (bit 12 should transition 0->1)
  // First verify the bit was cleared by reset
  Hwcfg2 = ReadReg (FalconBase + FALCON_HWCFG2);
  LogPrint (L"NVDAAL: HWCFG2 after reset: 0x%08X (scrub bit=%d)\n",
    Hwcfg2, (Hwcfg2 >> 12) & 1);

  // On Ada, check DMACTL scrubbing bits which might be more reliable
  {
    UINT32 Dmactl = ReadReg (FalconBase + FALCON_DMACTL);
    LogPrint (L"NVDAAL: DMACTL before scrub wait: 0x%08X (dmem=%d, imem=%d)\n",
      Dmactl, (Dmactl >> 1) & 1, (Dmactl >> 2) & 1);
  }

  // Wait for HWCFG2 scrubbing OR DMACTL scrubbing bits
  for (Timeout = 0; Timeout < 50000; Timeout++) {  // 50ms max
    gBS->Stall (1);  // 1us
    Hwcfg2 = ReadReg (FalconBase + FALCON_HWCFG2);
    UINT32 Dmactl = ReadReg (FalconBase + FALCON_DMACTL);
    // Check HWCFG2 bit 12 OR DMACTL bits 1&2 (dmem/imem scrubbing)
    if ((Hwcfg2 & (1 << 12)) || ((Dmactl & 0x6) == 0x6)) {
      break;
    }
  }
  LogPrint (L"NVDAAL: Scrub wait: %u us, HWCFG2=0x%08X, DMACTL=0x%08X\n",
    Timeout, Hwcfg2, ReadReg (FalconBase + FALCON_DMACTL));

  // Continue anyway - Ada might not set these bits the same way

  // Step 5: Select Falcon core if this is a dual RISCV/Falcon controller
  if (Hwcfg2 & (1 << 10)) {  // RISCV present
    LogPrint (L"NVDAAL: Selecting Falcon core (dual controller)...\n");

    // Read current state
    UINT32 BcrCtrl = ReadReg (FalconBase + FALCON_RISCV_BCR_CTRL);
    LogPrint (L"NVDAAL: BCR_CTRL before: 0x%08X\n", BcrCtrl);

    // Select Falcon core (bit 4 = 0 for Falcon)
    WriteReg (FalconBase + FALCON_RISCV_BCR_CTRL, 0);

    // Wait for valid bit
    for (Timeout = 0; Timeout < 10000; Timeout++) {
      gBS->Stall (1);
      BcrCtrl = ReadReg (FalconBase + FALCON_RISCV_BCR_CTRL);
      if (BcrCtrl & 1) {  // valid
        break;
      }
    }
    LogPrint (L"NVDAAL: Core select done after %u us, BCR_CTRL=0x%08X\n", Timeout, BcrCtrl);
  }

  // Step 6: Set RM register to PMC_BOOT_0 value
  {
    UINT32 Boot0 = ReadReg (NV_PMC_BOOT_0);
    WriteReg (FalconBase + FALCON_RM, Boot0);
    LogPrint (L"NVDAAL: Set RM register to 0x%08X\n", Boot0);
  }

  // Step 7: Configure FBIF for coherent system memory access with physical addresses
  LogPrint (L"NVDAAL: Configuring FBIF for DMA...\n");

  // Read current state first
  {
    UINT32 Dmactl = ReadReg (FalconBase + FALCON_DMACTL);
    UINT32 Transcfg = ReadReg (FalconBase + FALCON_FBIF_TRANSCFG);
    UINT32 FbifCtl = ReadReg (FalconBase + FALCON_FBIF_CTL);
    LogPrint (L"NVDAAL: Before config: DMACTL=0x%08X, TRANSCFG=0x%08X, FBIF_CTL=0x%08X\n",
      Dmactl, Transcfg, FbifCtl);
  }

  // Configure for physical addresses from coherent system memory
  // Note: DMACTL bit 0 = DMA enable (MUST be set for DMA to work!)
  WriteReg (FalconBase + FALCON_FBIF_CTL, 0x80);  // allow_phys_no_ctx (bit 7)
  WriteReg (FalconBase + FALCON_DMACTL, 0x1);     // Enable DMA engine (bit 0)
  WriteReg (FalconBase + FALCON_FBIF_TRANSCFG,
    FBIF_TRANSCFG_TARGET_COHERENT | FBIF_TRANSCFG_MEM_PHYSICAL);

  // Enable interfaces - ITFEN bit 2 enables FBIF
  WriteReg (FalconBase + FALCON_ITFEN, 0x4);
  LogPrint (L"NVDAAL: ITFEN set to 0x4\n");

  // Verify configuration
  {
    UINT32 Dmactl = ReadReg (FalconBase + FALCON_DMACTL);
    UINT32 Transcfg = ReadReg (FalconBase + FALCON_FBIF_TRANSCFG);
    UINT32 FbifCtl = ReadReg (FalconBase + FALCON_FBIF_CTL);
    UINT32 Itfen = ReadReg (FalconBase + FALCON_ITFEN);
    LogPrint (L"NVDAAL: After config: DMACTL=0x%08X, TRANSCFG=0x%08X, FBIF_CTL=0x%08X, ITFEN=0x%08X\n",
      Dmactl, Transcfg, FbifCtl, Itfen);
  }

  // Step 8: Set up DMA base address
  DmaBaseLo = (UINT32)(mDmaBuffer.PhysAddr >> 8);
  DmaBaseHi = (UINT32)(mDmaBuffer.PhysAddr >> 40);

  LogPrint (L"NVDAAL: Setting DMA base: 0x%08X (hi: 0x%02X)\n", DmaBaseLo, DmaBaseHi);
  WriteReg (FalconBase + FALCON_DMATRFBASE, DmaBaseLo);
  WriteReg (FalconBase + FALCON_DMATRFBASE1, DmaBaseHi);

  // Verify
  {
    UINT32 ReadBack = ReadReg (FalconBase + FALCON_DMATRFBASE);
    UINT32 ReadBackHi = ReadReg (FalconBase + FALCON_DMATRFBASE1);
    LogPrint (L"NVDAAL: DMATRFBASE readback: 0x%08X, BASE1: 0x%08X\n", ReadBack, ReadBackHi);
  }

  // Step 9: Transfer firmware to IMEM
  // Try DMA first, fall back to PIO if it fails
  LogPrint (L"NVDAAL: Loading firmware to IMEM (%u bytes)...\n", FirmwareSize);

  // Try DMA transfer first
  BOOLEAN DmaFailed = FALSE;
  for (Pos = 0; Pos < FirmwareSize; Pos += 256) {
    Status = DmaTransfer256 (FalconBase, Pos, Pos, TRUE, (Pos == 0));
    if (EFI_ERROR (Status)) {
      LogPrint (L"NVDAAL: DMA failed at offset 0x%X, trying PIO...\n", Pos);
      DmaFailed = TRUE;
      break;
    }
  }

  // If DMA failed, use PIO instead
  if (DmaFailed) {
    LogPrint (L"NVDAAL: Falling back to PIO mode\n");

    // Load directly from the firmware data (not DMA buffer)
    Status = LoadImemViaPio (
      FalconBase,
      (CONST UINT32 *)FirmwareData,
      (UINT32)FirmwareSize,
      0  // Start at offset 0
      );

    if (EFI_ERROR (Status)) {
      LogPrint (L"NVDAAL: PIO load also failed\n");
      FreeDmaBuffer ();
      return Status;
    }
    LogPrint (L"NVDAAL: IMEM loaded via PIO\n");
  } else {
    LogPrint (L"NVDAAL: IMEM loaded via DMA\n");
  }

  // Free DMA buffer - no longer needed
  FreeDmaBuffer ();

  // Step 10: Set boot vector
  LogPrint (L"NVDAAL: Setting boot vector: 0x%X\n", BootVec);
  WriteReg (FalconBase + FALCON_BOOTVEC, BootVec);

  // Verify
  {
    UINT32 ReadBack = ReadReg (FalconBase + FALCON_BOOTVEC);
    LogPrint (L"NVDAAL: BOOTVEC readback: 0x%08X\n", ReadBack);
  }

  // Step 11: Start Falcon execution
  LogPrint (L"NVDAAL: Starting Falcon execution...\n");

  // Check if we should use CPUCTL_ALIAS
  Cpuctl = ReadReg (FalconBase + FALCON_CPUCTL);
  LogPrint (L"NVDAAL: CPUCTL before start: 0x%08X (alias_en=%d)\n", Cpuctl, (Cpuctl >> 6) & 1);

  if (Cpuctl & (1 << 6)) {  // alias_en
    WriteReg (FalconBase + 0x130, FALCON_CPUCTL_STARTCPU);  // CPUCTL_ALIAS
  } else {
    WriteReg (FalconBase + FALCON_CPUCTL, FALCON_CPUCTL_STARTCPU);
  }

  // Step 12: Wait for completion
  LogPrint (L"NVDAAL: Waiting for completion...\n");

  Mailbox0 = 0;
  for (Timeout = 0; Timeout < 2000; Timeout++) {
    gBS->Stall (1000);  // 1ms

    Cpuctl = ReadReg (FalconBase + FALCON_CPUCTL);

    if (Cpuctl & FALCON_CPUCTL_HALTED) {
      Mailbox0 = ReadReg (FalconBase + FALCON_MAILBOX0);
      LogPrint (L"NVDAAL: Falcon halted after %u ms, CPUCTL=0x%08X, MB0=0x%08X\n",
             Timeout, Cpuctl, Mailbox0);
      break;
    }

    if ((Timeout % 500) == 0 && Timeout > 0) {
      LogPrint (L"NVDAAL: Still running... CPUCTL=0x%08X (%u ms)\n", Cpuctl, Timeout);
    }
  }

  if (Timeout >= 2000) {
    LogPrint (L"NVDAAL: Timeout waiting for Falcon\n");
    return EFI_TIMEOUT;
  }

  // Check result
  if (Mailbox0 != 0) {
    LogPrint (L"NVDAAL: Execution returned error: 0x%08X\n", Mailbox0);
    return EFI_DEVICE_ERROR;
  }

  LogPrint (L"NVDAAL: Execution completed successfully\n");
  return EFI_SUCCESS;
}

//=============================================================================
// Main Entry Point
//=============================================================================

EFI_STATUS
EFIAPI
NvdaalFwsecMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  UINT8       *VbiosData = NULL;
  UINTN       VbiosSize = 0;

  // Open log file first
  OpenLogFile (ImageHandle);

  LogPrint (L"\n");
  LogPrint (L"============================================\n");
  LogPrint (L"  NVDAAL FWSEC Executor v0.7\n");
  LogPrint (L"  For NVIDIA Ada Lovelace GPUs\n");
  LogPrint (L"  + NVIDIA-based FWSEC implementation\n");
  LogPrint (L"  + Proper fuse version & signature\n");
  LogPrint (L"  + BROM Interface (DMA-based HS mode)\n");
  LogPrint (L"  + File Logging to EFI partition\n");
  LogPrint (L"============================================\n\n");

  // Find GPU
  Status = FindNvidiaGpu ();
  if (EFI_ERROR (Status)) {
    LogPrint (L"NVDAAL: GPU not found, exiting\n");
    CloseLogFile ();
    return Status;
  }

  // Print initial status
  PrintGpuStatus (L"Initial GPU Status");

  // Check if WPR2 already configured
  if (IsWpr2Enabled ()) {
    LogPrint (L"\nNVDAAL: WPR2 already configured! Nothing to do.\n");
    CloseLogFile ();
    return EFI_SUCCESS;
  }

  LogPrint (L"\nNVDAAL: WPR2 not configured\n");

  //=========================================================================
  // METHOD 1: GPU Power Cycle (triggers Boot ROM -> FWSEC-FRTS)
  //=========================================================================
  LogPrint (L"\n*** METHOD 1: GPU Power Cycle ***\n");
  LogPrint (L"This triggers a cold boot which makes Boot ROM execute FWSEC\n\n");

  Status = GpuPowerCycle ();

  if (IsWpr2Enabled ()) {
    LogPrint (L"\n========================================\n");
    LogPrint (L"  WPR2 CONFIGURED VIA POWER CYCLE!\n");
    LogPrint (L"  GSP can now be booted in macOS\n");
    LogPrint (L"========================================\n");
    CloseLogFile ();
    return EFI_SUCCESS;
  }

  //=========================================================================
  // Try loading scrubber firmware from EFI partition first (more reliable)
  //=========================================================================
  Status = LoadScrubberFirmware ();
  if (!EFI_ERROR (Status) && mScrubberData != NULL) {
    LogPrint (L"\n*** Using scrubber firmware from file ***\n");

    // Try PIO method first (more reliable, bypasses DMA issues)
    LogPrint (L"NVDAAL: Attempting PIO (direct register) method...\n");
    Status = ExecuteScrubberViaPio (
      NV_PGSP_BASE,
      mScrubberData,
      mScrubberSize
      );

    // Check WPR2 after PIO attempt
    gBS->Stall (100000);  // Wait 100ms
    if (IsWpr2Enabled ()) {
      LogPrint (L"\nNVDAAL: *** WPR2 configured via PIO scrubber! ***\n");
      FreePool (mScrubberData);
      CloseLogFile ();
      return EFI_SUCCESS;
    }

    // If PIO didn't configure WPR2, try DMA method as fallback
    LogPrint (L"NVDAAL: PIO did not configure WPR2, trying DMA method...\n");
    Status = ExecuteFwsecViaBrom (
      NV_PGSP_BASE,
      mScrubberData,
      (UINT32)mScrubberSize,
      0  // Boot vector (scrubber starts at 0)
      );

    if (!EFI_ERROR (Status)) {
      // Check if WPR2 got configured
      gBS->Stall (100000);  // Wait 100ms
      if (IsWpr2Enabled ()) {
        LogPrint (L"\nNVDAAL: *** WPR2 configured via DMA scrubber! ***\n");
        FreePool (mScrubberData);
        CloseLogFile ();
        return EFI_SUCCESS;
      }
    }

    LogPrint (L"NVDAAL: Scrubber execution completed, WPR2 status will be checked below\n");
  } else {
    LogPrint (L"NVDAAL: Scrubber not found, falling back to VBIOS parsing...\n");
  }

  // Read VBIOS for subsequent methods
  Status = ReadVbiosFromGpu (&VbiosData, &VbiosSize);
  if (EFI_ERROR (Status)) {
    LogPrint (L"NVDAAL: Failed to read VBIOS\n");
    if (mScrubberData) FreePool (mScrubberData);
    CloseLogFile ();
    return Status;
  }

  // Parse VBIOS
  Status = ParseVbios (VbiosData, VbiosSize);
  if (EFI_ERROR (Status)) {
    LogPrint (L"NVDAAL: Failed to parse VBIOS (this is OK if scrubber was loaded)\n");
    // Don't fail here if scrubber was loaded
    if (mScrubberData == NULL) {
      FreePool (VbiosData);
      CloseLogFile ();
      return Status;
    }
  }

  //=========================================================================
  // METHOD 2: BROM Interface (DMA-based, proper HS mode verification)
  // Based on Linux nova-core implementation
  //=========================================================================
  LogPrint (L"\n*** METHOD 2: BROM Interface (DMA-based HS mode) ***\n");
  LogPrint (L"This uses Boot ROM to verify signature and execute in HS mode\n\n");

  if (mFwsecInfo.Valid && mFwsecInfo.StoredSize > 0) {
    // Build complete firmware blob for BROM: signature + IMEM + DMEM
    // The BROM expects the complete stored blob starting from signature
    UINT32 FwBlobStart = mFwsecInfo.SigOffset;
    UINT32 FwBlobSize = mFwsecInfo.StoredSize;

    // Validate blob bounds
    if (FwBlobStart + FwBlobSize <= VbiosSize) {
      LogPrint (L"NVDAAL: Firmware blob at 0x%X (size %u)\n", FwBlobStart, FwBlobSize);

      Status = ExecuteFwsecViaBrom (
        NV_PGSP_BASE,
        VbiosData + FwBlobStart,
        FwBlobSize,
        mFwsecInfo.BootVec
        );

      if (!EFI_ERROR (Status) && IsWpr2Enabled ()) {
        LogPrint (L"\nNVDAAL: *** WPR2 configured via BROM interface! ***\n");
        FreePool (VbiosData);
        CloseLogFile ();
        return EFI_SUCCESS;
      }
    } else {
      LogPrint (L"NVDAAL: Invalid firmware blob bounds (0x%X + %u > 0x%X)\n",
             FwBlobStart, FwBlobSize, VbiosSize);
    }
  } else {
    LogPrint (L"NVDAAL: StoredSize not available, cannot use BROM interface\n");
  }

  // Check WPR2 after BROM attempt
  if (IsWpr2Enabled ()) {
    LogPrint (L"NVDAAL: WPR2 enabled after BROM attempt\n");
    FreePool (VbiosData);
    CloseLogFile ();
    return EFI_SUCCESS;
  }

  //=========================================================================
  // METHOD 3: New Professional FWSEC Implementation
  // Based on NVIDIA open-gpu-kernel-modules (kernel_gsp_frts_tu102.c)
  //=========================================================================
  LogPrint (L"\n*** METHOD 3: Professional FWSEC-FRTS Implementation ***\n");
  LogPrint (L"Using NVIDIA-based implementation with proper signature patching\n\n");

  {
    // Calculate FRTS offset at top of VRAM (1MB from end)
    // Read usable FB size from register
    UINT32 FbSizeMb = ReadReg (0x00100A10);  // NV_USABLE_FB_SIZE_IN_MB
    UINT64 FrtsOffset;

    if (FbSizeMb == 0 || FbSizeMb == 0xFFFFFFFF || FbSizeMb > 64 * 1024) {
      // Default to 24GB - 1MB for RTX 4090 if register unavailable
      FbSizeMb = 24 * 1024;
      LogPrint (L"NVDAAL: Using default FB size: %u MB\n", FbSizeMb);
    } else {
      LogPrint (L"NVDAAL: FB size from register: %u MB\n", FbSizeMb);
    }

    FrtsOffset = ((UINT64)FbSizeMb << 20) - 0x100000;  // FB_SIZE - 1MB
    LogPrint (L"NVDAAL: FRTS offset: 0x%llX\n", FrtsOffset);

    // Test LogStr function (used by fwsec_impl.c)
    LogPrint (L"NVDAAL: Testing LogStr function...\n");
    LogStr (L"NVDAAL: [LogStr TEST] If you see this, LogStr works!\n");
    LogPrint (L"NVDAAL: LogStr test complete, calling FwsecExecuteFrts...\n");

    // Dump first 64 bytes of VBIOS for analysis
    LogPrint (L"NVDAAL: VBIOS header dump:\n");
    for (UINTN i = 0; i < 64; i += 16) {
      LogPrint (L"  %04X: %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X\n",
        i,
        VbiosData[i+0], VbiosData[i+1], VbiosData[i+2], VbiosData[i+3],
        VbiosData[i+4], VbiosData[i+5], VbiosData[i+6], VbiosData[i+7],
        VbiosData[i+8], VbiosData[i+9], VbiosData[i+10], VbiosData[i+11],
        VbiosData[i+12], VbiosData[i+13], VbiosData[i+14], VbiosData[i+15]);
    }

    // Dump around PMU table area (0x80DE0-0x80E10)
    if (VbiosSize > 0x80E10) {
      LogPrint (L"NVDAAL: PMU table area dump (0x80DE0):\n");
      for (UINTN i = 0x80DE0; i < 0x80E10; i += 16) {
        LogPrint (L"  %05X: %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X\n",
          i,
          VbiosData[i+0], VbiosData[i+1], VbiosData[i+2], VbiosData[i+3],
          VbiosData[i+4], VbiosData[i+5], VbiosData[i+6], VbiosData[i+7],
          VbiosData[i+8], VbiosData[i+9], VbiosData[i+10], VbiosData[i+11],
          VbiosData[i+12], VbiosData[i+13], VbiosData[i+14], VbiosData[i+15]);
      }
    }

    // Dump BIT header area (0x1A0-0x220)
    LogPrint (L"NVDAAL: BIT header area dump (0x1A0-0x220):\n");
    for (UINTN i = 0x1A0; i < 0x220; i += 16) {
      LogPrint (L"  %04X: %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X\n",
        i,
        VbiosData[i+0], VbiosData[i+1], VbiosData[i+2], VbiosData[i+3],
        VbiosData[i+4], VbiosData[i+5], VbiosData[i+6], VbiosData[i+7],
        VbiosData[i+8], VbiosData[i+9], VbiosData[i+10], VbiosData[i+11],
        VbiosData[i+12], VbiosData[i+13], VbiosData[i+14], VbiosData[i+15]);
    }

    // Dump FALCON_DATA area (0x410-0x450)
    LogPrint (L"NVDAAL: FALCON_DATA area dump (0x410-0x450):\n");
    for (UINTN i = 0x410; i < 0x450; i += 16) {
      LogPrint (L"  %04X: %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X\n",
        i,
        VbiosData[i+0], VbiosData[i+1], VbiosData[i+2], VbiosData[i+3],
        VbiosData[i+4], VbiosData[i+5], VbiosData[i+6], VbiosData[i+7],
        VbiosData[i+8], VbiosData[i+9], VbiosData[i+10], VbiosData[i+11],
        VbiosData[i+12], VbiosData[i+13], VbiosData[i+14], VbiosData[i+15]);
    }

    // Check for ASUS signature in first 0x200 bytes
    LogPrint (L"NVDAAL: Checking for vendor signatures...\n");
    for (UINTN i = 0; i < 0x200 - 4; i++) {
      if (VbiosData[i] == 'A' && VbiosData[i+1] == 'S' &&
          VbiosData[i+2] == 'U' && VbiosData[i+3] == 'S') {
        LogPrint (L"NVDAAL: Found 'ASUS' signature at offset 0x%X\n", i);
      }
      if (VbiosData[i] == 'R' && VbiosData[i+1] == 'O' && VbiosData[i+2] == 'G') {
        LogPrint (L"NVDAAL: Found 'ROG' signature at offset 0x%X\n", i);
      }
    }

    // Execute FWSEC-FRTS using new professional implementation
    LogPrint (L"NVDAAL: Calling FwsecExecuteFrts now...\n");
    Status = FwsecExecuteFrts (
      (UINT32)(UINTN)mMmioBase,  // BAR0 address
      VbiosData,
      VbiosSize,
      FrtsOffset
      );
    LogPrint (L"NVDAAL: FwsecExecuteFrts returned: %r\n", Status);
  }

  FreePool (VbiosData);

  // Print final status
  LogPrint (L"\n");
  PrintGpuStatus (L"Final GPU Status");

  if (IsWpr2Enabled ()) {
    LogPrint (L"\n============================================\n");
    LogPrint (L"  WPR2 CONFIGURED SUCCESSFULLY!\n");
    LogPrint (L"  GSP can now be booted in macOS\n");
    LogPrint (L"============================================\n");
  } else {
    LogPrint (L"\n============================================\n");
    LogPrint (L"  WPR2 NOT CONFIGURED\n");
    LogPrint (L"\n");
    LogPrint (L"  All methods attempted:\n");
    LogPrint (L"  1. Power Cycle - FAILED\n");
    LogPrint (L"  2. BROM Interface - FAILED\n");
    LogPrint (L"  3. Professional FWSEC - FAILED\n");
    LogPrint (L"\n");
    LogPrint (L"  The GPU's Boot ROM requires NVIDIA's\n");
    LogPrint (L"  cryptographic signature to execute\n");
    LogPrint (L"  FWSEC in Heavy-Secure mode.\n");
    LogPrint (L"\n");
    LogPrint (L"  This is a hardware security feature\n");
    LogPrint (L"  that cannot be bypassed.\n");
    LogPrint (L"============================================\n");
  }

  LogPrint (L"\nLog saved to EFI partition: NVDAAL_LOG.txt\n");

  // Close log file
  CloseLogFile ();

  return Status;
}
