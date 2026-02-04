/**
 * @file vbios.h
 * @brief NVIDIA VBIOS parsing structures for Ada Lovelace GPUs
 *
 * Based on Linux nova-core driver (vbios.rs)
 * Reference: https://docs.kernel.org/gpu/nova/core/vbios.html
 */

#ifndef NVDAAL_VBIOS_H
#define NVDAAL_VBIOS_H

#include <Uefi.h>

//
// VBIOS Constants
//
#define VBIOS_ROM_OFFSET            0x300000    // GPU ROM mapped address
#define VBIOS_ROM_SIGNATURE         0xAA55      // Standard ROM signature
#define VBIOS_PCIR_SIGNATURE        0x52494350  // "PCIR" in LE
#define VBIOS_NPDE_SIGNATURE        0x4544504E  // "NPDE" in LE
#define VBIOS_BIT_SIGNATURE         0x00544942  // "BIT\0" in LE
#define VBIOS_NVGI_SIGNATURE        0x4947564E  // "NVGI" in LE
#define VBIOS_RFRD_SIGNATURE        0x44524652  // "RFRD" in LE

//
// PCIR Code Types
//
#define PCIR_CODE_TYPE_X86          0x00
#define PCIR_CODE_TYPE_EFI          0x03
#define PCIR_CODE_TYPE_FWSEC        0xE0

//
// BIT Token IDs
//
#define BIT_TOKEN_NVINIT_PTRS       0x32
#define BIT_TOKEN_BIOSDATA          0x42
#define BIT_TOKEN_CLOCK_PTRS        0x43
#define BIT_TOKEN_DISPLAY_CTRL      0x44
#define BIT_TOKEN_I2C               0x49
#define BIT_TOKEN_MEM_PTRS          0x4D
#define BIT_TOKEN_NOP               0x4E
#define BIT_TOKEN_PERF_PTRS         0x50
#define BIT_TOKEN_STRING_PTRS       0x53
#define BIT_TOKEN_TMDS              0x54
#define BIT_TOKEN_FAN_TABLE         0x55
#define BIT_TOKEN_VOLTAGE_INFO      0x56
#define BIT_TOKEN_MEMORY_CONFIG     0x64
#define BIT_TOKEN_FALCON_DATA       0x70  // Points to PMU Lookup Table
#define BIT_TOKEN_UEFI              0x75
#define BIT_TOKEN_BRIDGE_FW         0x78

//
// PMU Application IDs
//
#define PMU_APP_ID_DEVINIT          0x01
#define PMU_APP_ID_SCRUBBER         0x07
#define PMU_APP_ID_SEC2             0x08
#define PMU_APP_ID_FWSEC_PROD       0x85
#define PMU_APP_ID_FWSEC_DBG        0x86

//
// PMU Target IDs
//
#define PMU_TARGET_PMU              0x01
#define PMU_TARGET_SEC2             0x05
#define PMU_TARGET_SCRUBBER         0x06
#define PMU_TARGET_GSP              0x07

#pragma pack(push, 1)

//
// ROM Header (at ROM signature 0x55AA)
//
typedef struct {
    UINT16  Signature;          // 0x55AA
    UINT8   Reserved[0x16];
    UINT16  PcirOffset;         // Offset to PCIR structure
} ROM_HEADER;

//
// PCI Data Structure (PCIR)
//
typedef struct {
    UINT32  Signature;          // "PCIR"
    UINT16  VendorId;           // 0x10DE for NVIDIA
    UINT16  DeviceId;
    UINT16  Reserved1;
    UINT16  Length;             // PCIR structure length
    UINT8   Revision;
    UINT8   ClassCode[3];
    UINT16  ImageLength;        // In 512-byte units
    UINT16  CodeRevision;
    UINT8   CodeType;           // 0x00=x86, 0x03=EFI, 0xE0=FWSEC
    UINT8   Indicator;          // Bit 7: last image
    UINT16  MaxRuntimeSize;
    UINT16  ConfigUtilityPtr;
    UINT16  DmtfClpPtr;
} PCIR_HEADER;

//
// NVGI Container Header
//
typedef struct {
    UINT32  Signature;          // "NVGI"
    UINT16  Version;
    UINT16  HeaderSize;
    UINT32  ImageSize;
    UINT32  Crc;
    UINT32  Flags;
    UINT8   Reserved[16];
} NVGI_HEADER;

//
// RFRD (ROM Firmware Reference Data) Header
//
typedef struct {
    UINT32  Signature;          // "RFRD"
    UINT16  Version;
    UINT16  HeaderSize;
    UINT32  DataOffset;         // Offset to ROM data (usually 0x9400)
    UINT32  DataSize;           // Size of ROM data
    UINT32  ImemOffset;
    UINT32  ImemSize;
    UINT32  DmemOffset;
    UINT32  DmemSize;
    UINT8   Reserved[8];
} RFRD_HEADER;

//
// BIT Header
//
typedef struct {
    UINT16  Prefix;             // Usually 0xB8FF
    UINT8   Signature[4];       // "BIT\0"
    UINT16  Version;
    UINT8   HeaderSize;
    UINT8   TokenSize;
    UINT8   TokenCount;
    UINT8   Reserved;
} BIT_HEADER;

//
// BIT Token Entry (6 bytes)
//
typedef struct {
    UINT8   Id;                 // Token type ID
    UINT8   Version;            // Token data version
    UINT16  DataSize;           // Size of token data
    UINT16  DataOffset;         // Offset from ROM base
} BIT_TOKEN;

//
// Falcon Data (pointed to by BIT_TOKEN_FALCON_DATA)
// Contains offset to PMU Lookup Table
//
typedef struct {
    UINT32  UcodeTableOffset;   // Offset to PMU Lookup Table
    UINT32  UcodeTableSize;     // Size (optional, may be 0)
} FALCON_DATA;

//
// PMU Lookup Table Header
//
typedef struct {
    UINT8   Version;            // Usually 0x01
    UINT8   HeaderSize;         // Usually 6
    UINT8   EntrySize;          // Usually 6
    UINT8   EntryCount;         // Number of entries
    UINT8   DescVersion;        // Descriptor version (unused)
    UINT8   Reserved;
} PMU_LOOKUP_TABLE_HEADER;

//
// PMU Lookup Table Entry (6 bytes)
//
typedef struct {
    UINT8   AppId;              // Application ID (0x85 = FWSEC_PROD)
    UINT8   TargetId;           // Target falcon ID
    UINT32  DataOffset;         // Offset to FalconUCodeDescV3
} PMU_LOOKUP_TABLE_ENTRY;

//
// NVIDIA Binary Header (nvfw_bin_hdr)
// Common header for firmware binaries
//
typedef struct {
    UINT16  VendorId;           // 0x10DE for NVIDIA
    UINT16  Version;            // Header version
    UINT32  Reserved;
    UINT32  TotalSize;          // Total firmware size
    UINT32  HeaderOffset;       // Offset to application header
    UINT32  HeaderSize;         // Size of application header
    UINT32  DataOffset;         // Offset to firmware data
    UINT32  DataSize;           // Size of firmware data
} NVFW_BIN_HDR;

//
// Falcon Ucode Descriptor V3 (FalconUCodeDescV3)
// Header for FWSEC and other falcon firmware
//
typedef struct {
    // Binary header (24 bytes)
    NVFW_BIN_HDR    BinHdr;

    // Descriptor fields
    UINT32  StoredSize;         // Stored size of ucode
    UINT32  PkcDataOffset;      // Offset to PKC signature data
    UINT32  InterfaceOffset;    // Offset to application interface
    UINT32  ImemPhysBase;       // IMEM physical base
    UINT32  ImemLoadSize;       // IMEM load size
    UINT32  ImemVirtBase;       // IMEM virtual base
    UINT32  DmemPhysBase;       // DMEM physical base
    UINT32  DmemLoadSize;       // DMEM load size
    UINT32  EngineIdMask;       // Engine ID mask for signatures
    UINT8   UcodeId;            // Ucode ID
    UINT8   SignatureCount;     // Number of signatures
    UINT16  SignatureVersions;  // Signature version bitmask
} FALCON_UCODE_DESC_V3;

#pragma pack(pop)

//
// VBIOS Context Structure
//
typedef struct {
    UINT8           *RomData;           // Pointer to ROM data
    UINTN           RomSize;            // Size of ROM data
    UINT32          RomBase;            // ROM base offset in VBIOS file

    // Parsed structures
    BIT_HEADER      *BitHeader;
    BIT_TOKEN       *BitTokens;
    UINT32          BitTokenCount;

    PMU_LOOKUP_TABLE_HEADER *PmuTable;
    PMU_LOOKUP_TABLE_ENTRY  *PmuEntries;
    UINT32          PmuEntryCount;

    // FWSEC info
    FALCON_UCODE_DESC_V3    *FwsecDesc;
    UINT32          FwsecOffset;        // Offset in ROM
    UINT32          FwsecSize;          // Total FWSEC size
} VBIOS_CONTEXT;

//
// Function Prototypes
//

/**
 * Initialize VBIOS context from ROM data
 */
EFI_STATUS
VbiosInit (
    OUT VBIOS_CONTEXT   *Context,
    IN  UINT8           *RomData,
    IN  UINTN           RomSize
    );

/**
 * Find ROM signature and base offset
 */
EFI_STATUS
VbiosFindRomBase (
    IN  VBIOS_CONTEXT   *Context,
    OUT UINT32          *RomBase
    );

/**
 * Find and parse BIT header
 */
EFI_STATUS
VbiosFindBitHeader (
    IN  VBIOS_CONTEXT   *Context
    );

/**
 * Get BIT token by ID
 */
EFI_STATUS
VbiosGetBitToken (
    IN  VBIOS_CONTEXT   *Context,
    IN  UINT8           TokenId,
    OUT BIT_TOKEN       **Token
    );

/**
 * Find and parse PMU Lookup Table
 */
EFI_STATUS
VbiosFindPmuTable (
    IN  VBIOS_CONTEXT   *Context
    );

/**
 * Get PMU entry by Application ID
 */
EFI_STATUS
VbiosGetPmuEntry (
    IN  VBIOS_CONTEXT   *Context,
    IN  UINT8           AppId,
    OUT PMU_LOOKUP_TABLE_ENTRY **Entry
    );

/**
 * Extract FWSEC firmware descriptor
 */
EFI_STATUS
VbiosExtractFwsec (
    IN  VBIOS_CONTEXT   *Context
    );

/**
 * Get FWSEC IMEM data
 */
EFI_STATUS
VbiosGetFwsecImem (
    IN  VBIOS_CONTEXT   *Context,
    OUT UINT8           **ImemData,
    OUT UINT32          *ImemSize
    );

/**
 * Get FWSEC DMEM data
 */
EFI_STATUS
VbiosGetFwsecDmem (
    IN  VBIOS_CONTEXT   *Context,
    OUT UINT8           **DmemData,
    OUT UINT32          *DmemSize
    );

/**
 * Get FWSEC signatures
 */
EFI_STATUS
VbiosGetFwsecSignatures (
    IN  VBIOS_CONTEXT   *Context,
    OUT UINT8           **SigData,
    OUT UINT32          *SigCount,
    OUT UINT32          *SigSize
    );

#endif // NVDAAL_VBIOS_H
