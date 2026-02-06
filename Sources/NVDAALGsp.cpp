/*
 * NVDAALGsp.cpp - GSP Controller Implementation
 *
 * Implements GSP initialization and RPC communication for Ada Lovelace GPUs.
 * Based on TinyGPU/tinygrad but rewritten for IOKit/macOS.
 */

#include "NVDAALGsp.h"
#include <libkern/libkern.h>
#include <libkern/OSByteOrder.h>

// ============================================================================
// Constructor / Destructor
// ============================================================================

NVDAALGsp::NVDAALGsp(void) {
    pciDevice = nullptr;
    mmioBase = nullptr;
    initialized = false;
    gspReady = false;
    rpcSeqNum = 0;
    lastHandle = 0;

    cmdQueueMem = nullptr;
    statQueueMem = nullptr;
    firmwareMem = nullptr;
    bootloaderMem = nullptr;
    booterLoadMem = nullptr;
    wprMetaMem = nullptr;
    radix3Mem = nullptr;
    fwsecMem = nullptr;
    vbiosMem = nullptr;
    vbiosPhys = 0;
    vbiosSize = 0;
    expansionRomOffset = 0;

    cmdQueue = nullptr;
    statQueue = nullptr;
    cmdQueueHead = 0;
    cmdQueueTail = 0;
    statQueueHead = 0;
    statQueueTail = 0;

    wpr2Lo = 0;
    wpr2Hi = 0;

    // FWSEC info
    memset(&fwsecInfo, 0, sizeof(fwsecInfo));
    fwsecImageOffset = 0;
    fwsecImageSize = 0;
}

NVDAALGsp::~NVDAALGsp(void) {
    free();
}

// ============================================================================
// Initialization
// ============================================================================

bool NVDAALGsp::init(IOPCIDevice *device, volatile uint32_t *mmio) {
    if (initialized) {
        IOLog("NVDAAL-GSP: Already initialized\n");
        return false;
    }

    pciDevice = device;
    mmioBase = mmio;

    // Read chip info
    uint32_t boot0 = readReg(NV_PMC_BOOT_0);
    uint32_t arch = (boot0 >> 20) & 0x1F;

    IOLog("NVDAAL-GSP: Chip architecture: 0x%02x\n", arch);

    if (arch != NV_CHIP_ARCH_ADA) {
        IOLog("NVDAAL-GSP: Warning - not Ada Lovelace (0x%02x), expected 0x19\n", arch);
    }

    // Check WPR2 status
    uint32_t wpr2Hi = readReg(NV_PFB_PRI_MMU_WPR2_ADDR_HI);
    if (NV_PFB_WPR2_ENABLED(wpr2Hi)) {
        IOLog("NVDAAL-GSP: WPR2 already active - need PCI reset\n");
        // TODO: Implement PCI reset
    }

    // Allocate command queue (host -> GSP)
    if (!allocDmaBuffer(&cmdQueueMem, QUEUE_SIZE, &cmdQueuePhys)) {
        IOLog("NVDAAL-GSP: Failed to allocate command queue\n");
        return false;
    }
    cmdQueue = (volatile uint8_t *)cmdQueueMem->getBytesNoCopy();
    memset((void *)cmdQueue, 0, QUEUE_SIZE);

    // Allocate status queue (GSP -> host)
    if (!allocDmaBuffer(&statQueueMem, QUEUE_SIZE, &statQueuePhys)) {
        IOLog("NVDAAL-GSP: Failed to allocate status queue\n");
        free();
        return false;
    }
    statQueue = (volatile uint8_t *)statQueueMem->getBytesNoCopy();
    memset((void *)statQueue, 0, QUEUE_SIZE);

    // Allocate WPR metadata buffer
    if (!allocDmaBuffer(&wprMetaMem, GSP_PAGE_SIZE, &wprMetaPhys)) {
        IOLog("NVDAAL-GSP: Failed to allocate WPR meta\n");
        free();
        return false;
    }

    IOLog("NVDAAL-GSP: Queues allocated\n");
    IOLog("NVDAAL-GSP:   cmdQueue  @ 0x%llx\n", cmdQueuePhys);
    IOLog("NVDAAL-GSP:   statQueue @ 0x%llx\n", statQueuePhys);

    initialized = true;
    return true;
}

void NVDAALGsp::free(void) {
    gspReady = false;
    initialized = false;

    freeDmaBuffer(&radix3Mem);
    freeDmaBuffer(&wprMetaMem);
    freeDmaBuffer(&fwsecMem);
    freeDmaBuffer(&vbiosMem);
    freeDmaBuffer(&booterLoadMem);
    freeDmaBuffer(&bootloaderMem);
    freeDmaBuffer(&firmwareMem);
    freeDmaBuffer(&statQueueMem);
    freeDmaBuffer(&cmdQueueMem);

    cmdQueue = nullptr;
    statQueue = nullptr;
    mmioBase = nullptr;
    pciDevice = nullptr;

    wpr2Lo = 0;
    wpr2Hi = 0;
}

// ============================================================================
// DMA Buffer Management
// ============================================================================

bool NVDAALGsp::allocDmaBuffer(IOBufferMemoryDescriptor **desc, size_t size, uint64_t *physAddr) {
    // Allocate physically contiguous, DMA-able memory
    *desc = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task,
        kIODirectionInOut | kIOMemoryPhysicallyContiguous,
        size,
        0xFFFFFFFFFFFFULL  // 48-bit physical address mask
    );

    if (!*desc) {
        return false;
    }

    IOReturn ret = (*desc)->prepare();
    if (ret != kIOReturnSuccess) {
        (*desc)->release();
        *desc = nullptr;
        return false;
    }

    // Get physical address
    *physAddr = (*desc)->getPhysicalSegment(0, nullptr);

    return true;
}

void NVDAALGsp::freeDmaBuffer(IOBufferMemoryDescriptor **desc) {
    if (*desc) {
        (*desc)->complete();
        (*desc)->release();
        *desc = nullptr;
    }
}

// ============================================================================
// Firmware Loading
// ============================================================================

bool NVDAALGsp::loadBootloader(const void *data, size_t size) {
    if (!initialized) {
        return false;
    }

    // Allocate memory for bootloader
    if (!allocDmaBuffer(&bootloaderMem, size, &bootloaderPhys)) {
        IOLog("NVDAAL-GSP: Failed to allocate bootloader memory\n");
        return false;
    }

    // Copy bootloader data
    memcpy(bootloaderMem->getBytesNoCopy(), data, size);

    IOLog("NVDAAL-GSP: Bootloader loaded (%lu bytes) @ 0x%llx\n",
          (unsigned long)size, bootloaderPhys);

    return true;
}

bool NVDAALGsp::loadBooterLoad(const void *data, size_t size) {
    if (!initialized) {
        return false;
    }

    // Free existing if any
    freeDmaBuffer(&booterLoadMem);

    // Allocate memory for booter_load (SEC2 ucode)
    if (!allocDmaBuffer(&booterLoadMem, size, &booterLoadPhys)) {
        IOLog("NVDAAL-GSP: Failed to allocate booter_load memory\n");
        return false;
    }

    // Copy booter data
    memcpy(booterLoadMem->getBytesNoCopy(), data, size);

    IOLog("NVDAAL-GSP: booter_load loaded (%lu bytes) @ 0x%llx\n",
          (unsigned long)size, booterLoadPhys);

    return true;
}

bool NVDAALGsp::loadVbios(const void *data, size_t size) {
    if (!initialized) {
        return false;
    }

    // Free existing if any
    freeDmaBuffer(&fwsecMem);

    // Allocate memory for VBIOS/FWSEC
    if (!allocDmaBuffer(&fwsecMem, size, &fwsecPhys)) {
        IOLog("NVDAAL-GSP: Failed to allocate VBIOS memory\n");
        return false;
    }

    // Copy VBIOS data
    memcpy(fwsecMem->getBytesNoCopy(), data, size);

    IOLog("NVDAAL-GSP: VBIOS loaded (%lu bytes) @ 0x%llx\n",
          (unsigned long)size, fwsecPhys);

    return true;
}

bool NVDAALGsp::readVbiosFromBar(void) {
    if (!initialized || !mmioBase) {
        IOLog("NVDAAL-GSP: Cannot read VBIOS - not initialized\n");
        return false;
    }

    IOLog("NVDAAL-GSP: Reading VBIOS from BAR0 @ 0x%x...\n", VBIOS_ROM_OFFSET);

    // Free existing if any
    freeDmaBuffer(&fwsecMem);

    // VBIOS can be up to 1MB - allocate full size and scan for actual size
    const size_t maxVbiosSize = 0x100000;  // 1MB max

    // Allocate buffer
    if (!allocDmaBuffer(&fwsecMem, maxVbiosSize, &fwsecPhys)) {
        IOLog("NVDAAL-GSP: Failed to allocate VBIOS buffer\n");
        return false;
    }

    uint8_t *vbios = (uint8_t *)fwsecMem->getBytesNoCopy();
    memset(vbios, 0, maxVbiosSize);

    // Read VBIOS from BAR0 in 32-bit chunks
    // VBIOS is at offset 0x300000 in BAR0
    for (size_t i = 0; i < maxVbiosSize; i += 4) {
        uint32_t val = mmioBase[(VBIOS_ROM_OFFSET + i) / 4];
        vbios[i + 0] = (val >> 0) & 0xFF;
        vbios[i + 1] = (val >> 8) & 0xFF;
        vbios[i + 2] = (val >> 16) & 0xFF;
        vbios[i + 3] = (val >> 24) & 0xFF;
    }

    // Verify ROM signature at first image
    // Note: The VBIOS may have a header before the actual ROM images
    // Search for first 0x55AA signature
    uint32_t romStart = 0;
    for (uint32_t off = 0; off < 0x10000; off += 512) {
        if (vbios[off] == 0x55 && vbios[off + 1] == 0xAA) {
            romStart = off;
            IOLog("NVDAAL-GSP: Found ROM signature at offset 0x%x\n", romStart);
            break;
        }
    }

    if (romStart == 0 && (vbios[0] != 0x55 || vbios[1] != 0xAA)) {
        // Check for NVGI header (some VBIOSes have this)
        if (vbios[0] == 'N' && vbios[1] == 'V' && vbios[2] == 'G' && vbios[3] == 'I') {
            IOLog("NVDAAL-GSP: VBIOS has NVGI header, scanning for ROM images...\n");
            // NVGI header present - ROM images are somewhere after
            for (uint32_t off = 0x1000; off < 0x10000; off += 512) {
                if (vbios[off] == 0x55 && vbios[off + 1] == 0xAA) {
                    romStart = off;
                    IOLog("NVDAAL-GSP: Found ROM signature at offset 0x%x\n", romStart);
                    break;
                }
            }
        }
    }

    // Scan for images and count them
    int imageCount = 0;
    int fwsecCount = 0;
    uint32_t offset = romStart;
    size_t actualSize = 0;

    while (offset < maxVbiosSize - 32) {
        if (vbios[offset] == 0x55 && vbios[offset + 1] == 0xAA) {
            // Get PCIR offset
            uint16_t pcirOff = vbios[offset + 0x18] | (vbios[offset + 0x19] << 8);
            uint32_t pcirAddr = offset + pcirOff;

            if (pcirAddr + 24 > maxVbiosSize) break;

            // Verify PCIR signature
            if (vbios[pcirAddr] == 'P' && vbios[pcirAddr + 1] == 'C' &&
                vbios[pcirAddr + 2] == 'I' && vbios[pcirAddr + 3] == 'R') {

                uint8_t codeType = vbios[pcirAddr + 20];
                uint16_t imageLen = vbios[pcirAddr + 16] | (vbios[pcirAddr + 17] << 8);
                uint8_t lastImage = vbios[pcirAddr + 21];

                IOLog("NVDAAL-GSP: Image %d @ 0x%x: type=0x%02x, size=%u bytes%s\n",
                      imageCount, offset, codeType, imageLen * 512,
                      (lastImage & 0x80) ? " (LAST)" : "");

                if (codeType == VBIOS_IMAGE_TYPE_FWSEC) {
                    fwsecCount++;
                    IOLog("NVDAAL-GSP: >>> Found FWSEC image #%d!\n", fwsecCount);
                }

                imageCount++;
                actualSize = offset + imageLen * 512;

                // Check if last image
                if (lastImage & 0x80) {
                    break;
                }

                offset += imageLen * 512;
                // Align to 512-byte boundary
                offset = (offset + 511) & ~511;
            } else {
                offset += 512;
            }
        } else {
            offset += 512;
        }
    }

    IOLog("NVDAAL-GSP: VBIOS read complete: %d images found, %d FWSEC images\n",
          imageCount, fwsecCount);

    if (fwsecCount == 0) {
        IOLog("NVDAAL-GSP: WARNING - No FWSEC images found in VBIOS!\n");
        IOLog("NVDAAL-GSP: FWSEC may be in PMU Lookup Table or WPR2 set by EFI\n");
    }

    return imageCount > 0;
}

// ============================================================================
// PROM Reading - Read VBIOS directly from GPU PROM registers (after POST)
// This method reads VBIOS from BAR0 + 0x180000 (NV_PROM_DATA)
// After GPU POST, the FWSEC firmware is available unencrypted here
// ============================================================================

uint32_t NVDAALGsp::readPromData(uint32_t offset) {
    // Read 32-bit value from PROM space
    // NV_PROM_DATA(offset) = BAR0 + 0x180000 + offset
    return readReg(NV_PROM_DATA(offset));
}

bool NVDAALGsp::locateExpansionRoms(uint32_t *biosSize, uint32_t *expRomOffset) {
    // Locate expansion ROMs in PROM and calculate expansionRomOffset
    // This offset is needed to properly parse FWSEC from BIT Token 0x70

    uint32_t currBlock = 0;  // Start at beginning of PROM
    uint32_t extRomOffset = 0;
    uint32_t baseRomSize = 0;
    uint32_t lastOffset = 0;
    uint32_t lastSize = 0;

    IOLog("NVDAAL-GSP: Scanning PROM for expansion ROMs...\n");

    // Find all ROMs by following PCIR structures
    for (int i = 0; i < 16; i++) {  // Max 16 images
        // Read ROM signature
        uint16_t romSig = (uint16_t)readPromData(currBlock);
        if (romSig != PCI_ROM_SIGNATURE) {
            if (i == 0) {
                IOLog("NVDAAL-GSP: No valid ROM signature at PROM start\n");
                return false;
            }
            break;
        }

        // Get PCIR offset
        uint16_t pcirOffset = (uint16_t)readPromData(currBlock + PCI_ROM_PCIR_OFFSET);
        uint32_t pcirAddr = currBlock + pcirOffset;

        // Verify PCIR signature
        uint32_t pcirSig = readPromData(pcirAddr);
        if (pcirSig != PCIR_SIGNATURE) {
            IOLog("NVDAAL-GSP: Invalid PCIR signature at 0x%x: 0x%08x\n", pcirAddr, pcirSig);
            break;
        }

        // Read image length (in 512-byte units) and code type
        uint16_t imageLen = (uint16_t)(readPromData(pcirAddr + PCIR_IMAGE_LEN_OFFSET) & 0xFFFF);
        uint8_t codeType = (uint8_t)(readPromData(pcirAddr + PCIR_CODE_TYPE_OFFSET) & 0xFF);
        uint8_t indicator = (uint8_t)((readPromData(pcirAddr + PCIR_CODE_TYPE_OFFSET) >> 8) & 0xFF);

        uint32_t imageSize = imageLen * PCI_ROM_IMAGE_BLOCK_SIZE;

        IOLog("NVDAAL-GSP: PROM Image %d @ 0x%x: type=0x%02x, size=%u bytes%s\n",
              i, currBlock, codeType, imageSize,
              (indicator & PCIR_LAST_IMAGE_FLAG) ? " (LAST)" : "");

        // Track FWSEC extension ROM offset (type 0xE0)
        if (extRomOffset == 0 && codeType == NV_VBIOS_CODE_TYPE_FWSEC) {
            extRomOffset = currBlock;
            IOLog("NVDAAL-GSP: Found FWSEC extension ROM at offset 0x%x\n", extRomOffset);
        }

        // Track base ROM size (type 0x00)
        if (baseRomSize == 0 && codeType == NV_VBIOS_CODE_TYPE_PCIAT) {
            baseRomSize = imageSize;
        }

        lastOffset = currBlock;
        lastSize = imageSize;

        // Check if last image
        if (indicator & PCIR_LAST_IMAGE_FLAG) {
            break;
        }

        // Move to next image
        currBlock += imageSize;
    }

    // Return total BIOS size
    if (biosSize) {
        *biosSize = lastOffset + lastSize;
    }

    // Calculate expansionRomOffset as Linux driver does:
    // expansionRomOffset = extRomOffset - baseRomSize (when both are non-zero)
    if (expRomOffset) {
        if (extRomOffset > 0 && baseRomSize > 0) {
            *expRomOffset = extRomOffset - baseRomSize;
            IOLog("NVDAAL-GSP: Calculated expansionRomOffset = 0x%x\n", *expRomOffset);
        } else {
            *expRomOffset = 0;
        }
    }

    return true;
}

bool NVDAALGsp::readVbiosFromProm(void) {
    IOLog("NVDAAL-GSP: Reading VBIOS from PROM (BAR0 + 0x%x)...\n", NV_PROM_BASE);

    // ========================================================================
    // Timing verification (per HuggingChat/NVIDIA documentation):
    // 1. Verify GPU has completed POST via NV_PMC_ENABLE
    // 2. Check BAR0 is active via NV_PBUS_PCI_NV_19
    // 3. Wait 50-100ms after GPU readiness
    // 4. Retry with backoff if PROM returns all-FFs
    // ========================================================================

    // Step 1: Check POST completion - NV_PMC_ENABLE should have engine bits set
    uint32_t pmcEnable = readReg(NV_PMC_ENABLE);
    IOLog("NVDAAL-GSP: NV_PMC_ENABLE = 0x%08x\n", pmcEnable);
    if (pmcEnable == 0 || pmcEnable == 0xFFFFFFFF) {
        IOLog("NVDAAL-GSP: GPU may not have completed POST (PMC_ENABLE=0x%08x)\n", pmcEnable);
        // Continue anyway - macOS might handle POST differently
    }

    // Step 2: Check BAR0 state
    uint32_t pciNv19 = readReg(NV_PBUS_PCI_NV_19);
    IOLog("NVDAAL-GSP: NV_PBUS_PCI_NV_19 = 0x%08x\n", pciNv19);

    // Step 3: Wait for GPU to be ready (50-100ms recommended)
    IOLog("NVDAAL-GSP: Waiting 100ms for GPU PROM readiness...\n");
    IOSleep(100);

    // Step 4: First read to check if PROM is accessible (with retry)
    uint32_t testRead = readPromData(0);
    int retryCount = 0;
    const int maxRetries = 5;

    while (testRead == 0xFFFFFFFF && retryCount < maxRetries) {
        IOLog("NVDAAL-GSP: PROM returned 0xFFFFFFFF, retry %d/%d with backoff...\n",
              retryCount + 1, maxRetries);
        IOSleep(50 * (retryCount + 1));  // Exponential backoff: 50, 100, 150, 200, 250ms
        testRead = readPromData(0);
        retryCount++;
    }

    if (testRead == 0xFFFFFFFF) {
        IOLog("NVDAAL-GSP: PROM still returning 0xFFFFFFFF after %d retries - access premature?\n", maxRetries);
        // Continue anyway - let locateExpansionRoms handle validation
    } else {
        IOLog("NVDAAL-GSP: PROM accessible, first word = 0x%08x\n", testRead);
    }

    // First, locate expansion ROMs to determine BIOS size and expansion ROM offset
    uint32_t biosSize = 0;
    if (!locateExpansionRoms(&biosSize, &expansionRomOffset)) {
        IOLog("NVDAAL-GSP: Failed to locate expansion ROMs in PROM\n");
        return false;
    }

    if (biosSize == 0 || biosSize > NV_VBIOS_MAX_SIZE) {
        IOLog("NVDAAL-GSP: Invalid BIOS size from PROM: %u\n", biosSize);
        return false;
    }

    IOLog("NVDAAL-GSP: VBIOS size from PROM: %u bytes, expansionRomOffset: 0x%x\n",
          biosSize, expansionRomOffset);

    // Allocate buffer for VBIOS
    if (!allocDmaBuffer(&vbiosMem, biosSize, &vbiosPhys)) {
        IOLog("NVDAAL-GSP: Failed to allocate VBIOS buffer\n");
        return false;
    }

    uint8_t *vbiosData = (uint8_t *)vbiosMem->getBytesNoCopy();
    vbiosSize = biosSize;

    // Read VBIOS from PROM using 32-bit aligned reads
    IOLog("NVDAAL-GSP: Copying VBIOS from PROM...\n");
    uint32_t alignedSize = (biosSize + 3) & ~3;  // Align to 4 bytes

    for (uint32_t i = 0; i < alignedSize; i += 4) {
        uint32_t val = readPromData(i);
        if (i + 4 <= biosSize) {
            *((uint32_t *)(vbiosData + i)) = val;
        } else {
            // Handle last partial word
            for (uint32_t j = i; j < biosSize; j++) {
                vbiosData[j] = (uint8_t)(val >> ((j - i) * 8));
            }
        }
    }

    // Verify we got valid data
    if (vbiosData[0] != 0x55 || vbiosData[1] != 0xAA) {
        IOLog("NVDAAL-GSP: WARNING: PROM data doesn't have valid ROM signature (got 0x%02x%02x)\n",
              vbiosData[0], vbiosData[1]);
        // Continue anyway - some VBIOSes have different header formats
    } else {
        IOLog("NVDAAL-GSP: VBIOS ROM signature verified (0x55AA)\n");
    }

    // Log first few bytes for debugging
    IOLog("NVDAAL-GSP: PROM data: %02x %02x %02x %02x %02x %02x %02x %02x\n",
          vbiosData[0], vbiosData[1], vbiosData[2], vbiosData[3],
          vbiosData[4], vbiosData[5], vbiosData[6], vbiosData[7]);

    IOLog("NVDAAL-GSP: VBIOS read from PROM complete (%u bytes)\n", vbiosSize);

    return true;
}

// ============================================================================
// VBIOS / FWSEC Parsing
// ============================================================================

bool NVDAALGsp::parseVbios(const void *vbios, size_t size) {
    const uint8_t *data = (const uint8_t *)vbios;
    
    IOLog("NVDAAL-GSP: Parsing VBIOS (%lu bytes)...\n", (unsigned long)size);
    
    // Step 1: Find FWSEC image (type 0xE0) by scanning for 0x55AA signatures
    uint32_t offset = 0;
    uint32_t fwsecStart = 0;
    uint32_t fwsecLen = 0;
    int imageCount = 0;
    
    while (offset < size - 2) {
        // Look for ROM signature at 512-byte boundaries
        if ((offset & 0x1FF) == 0 && data[offset] == 0x55 && data[offset + 1] == 0xAA) {
            const VbiosRomHeader *romHdr = (const VbiosRomHeader *)(data + offset);
            
            if (romHdr->pciDataOffset == 0 || offset + romHdr->pciDataOffset + sizeof(VbiosPcirHeader) > size) {
                offset += 512;
                continue;
            }
            
            const VbiosPcirHeader *pcir = (const VbiosPcirHeader *)(data + offset + romHdr->pciDataOffset);
            
            // Verify PCIR signature
            if (pcir->signature != 0x52494350) {  // "PCIR" little-endian
                offset += 512;
                continue;
            }
            
            uint32_t imageLen = pcir->imageLength * 512;
            
            IOLog("NVDAAL-GSP: Found image %d at 0x%x: type=0x%02x, len=%u\n",
                  imageCount, offset, pcir->codeType, imageLen);
            
            // Check if this is FWSEC image
            if (pcir->codeType == VBIOS_IMAGE_TYPE_FWSEC) {
                if (fwsecStart == 0) {
                    fwsecStart = offset;
                    fwsecLen = imageLen;
                    IOLog("NVDAAL-GSP: Found first FWSEC image at 0x%x\n", fwsecStart);
                }
            }
            
            imageCount++;
            
            // Check if last image
            if (pcir->indicator & 0x80) {
                break;
            }
            
            offset += imageLen;
        } else {
            offset += 512;
        }
    }
    
    if (fwsecStart == 0) {
        // On Ada Lovelace, FWSEC is NOT a PCI ROM image (codeType 0xE0).
        // Instead, FWSEC is embedded in VBIOS and found via BIT Falcon Ucode Table:
        // BIT header → Token 0x70 → Falcon Ucode Table → AppID 0x85
        // Confirmed via nouveau fwsec.c and open-gpu-kernel-modules kernel_gsp_fwsec.c
        IOLog("NVDAAL-GSP: No FWSEC PCI ROM image (0xE0) - will use BIT table (Token 0x70, AppID 0x85)\n");
    } else {
        fwsecImageOffset = fwsecStart;
        fwsecImageSize = fwsecLen;
    }
    
    // Step 2: Find BIT header in VBIOS (usually in first image)
    // Also find the base offset of the image containing BIT
    const uint8_t bitPattern[] = {0xFF, 0xB8, 'B', 'I', 'T', 0x00};
    uint32_t bitOffset = 0;
    uint32_t imageBase = 0;  // Base offset of image containing BIT
    
    // First, find image boundaries
    uint32_t lastImageStart = 0;
    for (uint32_t i = 0; i < size - 2; i += 512) {
        if (data[i] == 0x55 && data[i + 1] == 0xAA) {
            lastImageStart = i;
        }
    }
    
    for (uint32_t i = 0; i < size - 6; i++) {
        if (memcmp(data + i, bitPattern, 6) == 0) {
            bitOffset = i;
            // Find which image contains this BIT
            for (uint32_t j = 0; j < size - 2; j += 512) {
                if (data[j] == 0x55 && data[j + 1] == 0xAA && j <= bitOffset) {
                    imageBase = j;
                }
            }
            break;
        }
    }
    
    if (bitOffset == 0) {
        IOLog("NVDAAL-GSP: BIT header not found\n");
        fwsecInfo.valid = false;
        return true;
    }
    
    IOLog("NVDAAL-GSP: Found BIT header at 0x%x (image base 0x%x)\n", bitOffset, imageBase);
    
    const BitHeader *bit = (const BitHeader *)(data + bitOffset);

    // Step 3: Scan BIT tokens for Falcon Ucode Table
    // Token 0x70 (FALCON_DATA) → Falcon Ucode Table containing FWSEC (AppID 0x85)
    // Token 0x50 (PMU_TABLE) → Alternative PMU table offsets (Ada may use either/both)
    uint32_t tokenOffset = bitOffset + bit->headerSize;
    uint32_t pmuTokenOffset = 0;
    uint32_t falconDataOffset = 0;
    uint16_t pmuTokenDataSize = 0;

    for (int i = 0; i < bit->tokenCount && tokenOffset < size - sizeof(BitToken); i++) {
        const BitToken *token = (const BitToken *)(data + tokenOffset);

        if (token->id == BIT_TOKEN_PMU_TABLE) {
            // Token 0x50: Ada Lovelace PMU table offsets
            pmuTokenOffset = imageBase + token->dataOffset;
            pmuTokenDataSize = token->dataSize;
            IOLog("NVDAAL-GSP: Found PMU Table token (0x50) at 0x%x (rel 0x%x), size=%d\n",
                  pmuTokenOffset, token->dataOffset, pmuTokenDataSize);
        } else if (token->id == BIT_TOKEN_FALCON_DATA) {
            // Token 0x70: Pre-Ada Falcon ucode table
            falconDataOffset = imageBase + token->dataOffset;
            IOLog("NVDAAL-GSP: Found Falcon Data token (0x70) at 0x%x (rel 0x%x)\n",
                  falconDataOffset, token->dataOffset);
        }

        tokenOffset += bit->tokenSize;
    }

    uint32_t pmuTableOffset = 0;
    const PmuLookupTableHeader *pmuHdr = nullptr;

    // Step 4: Ada Lovelace path - use Token 0x50 directly
    if (pmuTokenOffset != 0 && pmuTokenDataSize >= 2) {
        IOLog("NVDAAL-GSP: Using Ada Lovelace Token 0x50 path for PMU table\n");

        if (pmuTokenOffset + pmuTokenDataSize > size) {
            IOLog("NVDAAL-GSP: Invalid PMU token offset\n");
            return false;
        }

        // Token 0x50 format: Raw array of 32-bit offsets (NO header in Ada Lovelace!)
        // Each offset may point to various tables - we check each for PMU signature
        const uint8_t *pmuTokenData = data + pmuTokenOffset;
        const uint32_t *offsets = (const uint32_t *)pmuTokenData;
        int numOffsets = pmuTokenDataSize / 4;

        IOLog("NVDAAL-GSP: Token 0x50: %d potential offsets\n", numOffsets);

        // Try each offset to find valid PMU table
        for (int i = 0; i < numOffsets && i < 64 && pmuTableOffset == 0; i++) {
            uint32_t candidateOffset = offsets[i];

            // Skip zero or invalid offsets
            if (candidateOffset == 0 || candidateOffset + sizeof(PmuLookupTableHeader) > size) {
                continue;
            }

            const PmuLookupTableHeader *testHdr = (const PmuLookupTableHeader *)(data + candidateOffset);

            // Validate PMU table header signature: version=1, headerSize=6, entrySize=6
            if (testHdr->version == PMU_TABLE_SIGNATURE_V1 &&
                testHdr->headerSize == PMU_TABLE_HEADER_SIZE_V1 &&
                testHdr->entrySize == PMU_TABLE_ENTRY_SIZE_V1 &&
                testHdr->entryCount >= 1 && testHdr->entryCount <= 32) {

                IOLog("NVDAAL-GSP: Found valid PMU table at 0x%x via Token 0x50 (entry %d)\n",
                      candidateOffset, i);
                pmuTableOffset = candidateOffset;
                pmuHdr = testHdr;
                break;
            }
        }

        if (pmuTableOffset == 0) {
            IOLog("NVDAAL-GSP: Token 0x50 offsets don't contain PMU table, will use pattern search\n");
        }
    }

    // Step 4b: Pre-Ada fallback - use Token 0x70 (FALCON_DATA)
    if (pmuTableOffset == 0 && falconDataOffset != 0) {
        if (falconDataOffset + sizeof(BitFalconData) > size) {
            IOLog("NVDAAL-GSP: Invalid Falcon Data offset\n");
            return false;
        }

        const BitFalconData *falconData = (const BitFalconData *)(data + falconDataOffset);
        pmuTableOffset = imageBase + falconData->ucodeTableOffset;

        if (pmuTableOffset + sizeof(PmuLookupTableHeader) <= size) {
            pmuHdr = (const PmuLookupTableHeader *)(data + pmuTableOffset);
        }
    }

    // Step 5: Validate PMU table or search by pattern
    if (pmuTableOffset == 0 || pmuHdr == nullptr) {
        IOLog("NVDAAL-GSP: No PMU table found via BIT tokens\n");
        fwsecInfo.valid = false;
        return true;
    }

    IOLog("NVDAAL-GSP: PMU Table: version=%d, entries=%d, entrySize=%d, headerSize=%d\n",
          pmuHdr->version, pmuHdr->entryCount, pmuHdr->entrySize, pmuHdr->headerSize);

    // If PMU table looks invalid, search for it by pattern
    if (pmuHdr->entryCount == 0 || pmuHdr->version > 10 || pmuHdr->entrySize < 4 || pmuHdr->entrySize > 200) {
        IOLog("NVDAAL-GSP: PMU table looks invalid, searching by pattern...\n");

        // Search for PMU table pattern: version 1, headerSize 6, entrySize 6 (Ada signature)
        bool found = false;
        for (uint32_t searchOffset = 0x9000; searchOffset < size - 0x100 && !found; searchOffset += 4) {
            const PmuLookupTableHeader *testHdr = (const PmuLookupTableHeader *)(data + searchOffset);

            // Check for Ada Lovelace PMU table signature: 01 06 06 xx
            if (testHdr->version == 1 && testHdr->headerSize == 6 && testHdr->entrySize == 6 &&
                testHdr->entryCount >= 1 && testHdr->entryCount <= 32) {

                // Verify entries contain FWSEC (appId 0x85)
                uint32_t testEntryOffset = searchOffset + testHdr->headerSize;
                for (int i = 0; i < testHdr->entryCount && testEntryOffset + testHdr->entrySize <= size; i++) {
                    const PmuLookupEntry *testEntry = (const PmuLookupEntry *)(data + testEntryOffset);
                    if (testEntry->appId == 0x85) {
                        IOLog("NVDAAL-GSP: Found valid PMU table at 0x%x with FWSEC entry!\n", searchOffset);
                        pmuTableOffset = searchOffset;
                        pmuHdr = (const PmuLookupTableHeader *)(data + pmuTableOffset);
                        found = true;
                        break;
                    }
                    testEntryOffset += testHdr->entrySize;
                }
            }
        }

        if (!found) {
            IOLog("NVDAAL-GSP: Could not find valid PMU table by search\n");
        }
    }
    
    uint32_t entryOffset = pmuTableOffset + pmuHdr->headerSize;

    // 6-byte entries can be EITHER format - check both!
    // Pre-Ada: appId(1) + targetId(1) + offset(4) = 6 bytes
    // Ada: appId(2) + offset(4) = 6 bytes
    // We'll try pre-Ada format first (1-byte appId) since this VBIOS uses it
    IOLog("NVDAAL-GSP: PMU entrySize=%d - checking both entry formats\n", pmuHdr->entrySize);

    for (int i = 0; i < pmuHdr->entryCount && entryOffset < size - pmuHdr->entrySize; i++) {
        // Always parse as pre-Ada format first (1-byte appId)
        const PmuLookupEntry *entry = (const PmuLookupEntry *)(data + entryOffset);
        uint8_t appId8 = entry->appId;
        uint32_t dataOffset = entry->dataOffset;

        IOLog("NVDAAL-GSP: PMU Entry %d: appId=0x%02x, targetId=0x%02x, dataOff=0x%x\n",
              i, appId8, entry->targetId, dataOffset);

        // Check pre-Ada format (1-byte appId = 0x85)
        bool foundFwsec = (appId8 == FWSEC_APP_ID_FWSEC || appId8 == 0x01);

        // Also check Ada format (2-byte appId = 0x0085)
        if (!foundFwsec && pmuHdr->entrySize == 6) {
            const PmuLookupEntryAda *entryAda = (const PmuLookupEntryAda *)(data + entryOffset);
            if (entryAda->appId == 0x0085 || entryAda->appId == 0x0001) {
                foundFwsec = true;
                dataOffset = entryAda->dataOffset;
                IOLog("NVDAAL-GSP: PMU Entry %d (Ada format): appId=0x%04x, dataOff=0x%x\n",
                      i, entryAda->appId, dataOffset);
            }
        }

        // Look for FWSEC app
        if (foundFwsec) {
            uint32_t ucodeOffset = dataOffset;
            
            // Adjust offset - may be relative to FWSEC image
            if (ucodeOffset < fwsecStart) {
                ucodeOffset += fwsecStart;
            }
            
            if (ucodeOffset + sizeof(FalconUcodeDescV3) > size) {
                continue;
            }
            
            // Check if there's a NVFW binary header before the ucode descriptor
            // The binary header has vendorId=0x10DE at the start
            const NvfwBinHdr *binHdr = (const NvfwBinHdr *)(data + ucodeOffset);
            uint32_t ucodeDescOffset = ucodeOffset;
            uint32_t storedSize = 0;

            if (binHdr->vendorId == 0x10DE && binHdr->version >= 1 && binHdr->version <= 0x10) {
                // We have a binary header - storedSize comes after it
                IOLog("NVDAAL-GSP: Found NVFW_BIN_HDR: vendorId=0x%04x ver=%d totalSize=0x%x\n",
                      binHdr->vendorId, binHdr->version, binHdr->totalSize);

                // StoredSize is at offset 0x18 (after the 24-byte BinHdr)
                storedSize = *(const uint32_t *)(data + ucodeOffset + sizeof(NvfwBinHdr));
                ucodeDescOffset = ucodeOffset + binHdr->headerOffset;

                IOLog("NVDAAL-GSP: StoredSize=0x%x, ucodeDesc at 0x%x\n", storedSize, ucodeDescOffset);
            }

            if (ucodeDescOffset + sizeof(FalconUcodeDescV3) > size) {
                continue;
            }

            const FalconUcodeDescV3 *ucode = (const FalconUcodeDescV3 *)(data + ucodeDescOffset);

            IOLog("NVDAAL-GSP: Ucode Desc: imemOff=0x%x imemSz=0x%x dmemOff=0x%x dmemSz=0x%x\n",
                  ucode->imemOffset, ucode->imemSize, ucode->dmemOffset, ucode->dmemSize);

            // Validate descriptor has reasonable values
            bool validDescriptor = (ucode->imemSize > 0 && ucode->imemSize < 0x80000 &&
                                    ucode->dmemSize > 0 && ucode->dmemSize < 0x80000 &&
                                    ucode->dataSize > 0 && ucode->dataSize < 0x100000);

            if (!validDescriptor && storedSize == 0) {
                IOLog("NVDAAL-GSP: Invalid descriptor at offset 0x%x (no NVFW header), skipping\n",
                      ucodeOffset);
                continue;  // Skip this entry, try next PMU entry
            }

            // Store FWSEC info
            fwsecInfo.fwOffset = ucodeOffset;  // Original offset for DMA loading
            fwsecInfo.storedSize = storedSize > 0 ? storedSize : ucode->dataSize;
            fwsecInfo.imemOffset = ucodeDescOffset + ucode->imemOffset;
            fwsecInfo.imemSize = ucode->imemSize;
            fwsecInfo.imemSecSize = ucode->imemSecureSize;
            fwsecInfo.dmemOffset = ucodeDescOffset + ucode->dmemOffset;
            fwsecInfo.dmemSize = ucode->dmemSize;
            fwsecInfo.sigOffset = ucodeDescOffset + ucode->sigOffset;
            fwsecInfo.sigSize = ucode->sigSize;
            fwsecInfo.bootVec = ucode->bootVec;
            fwsecInfo.valid = validDescriptor || (storedSize > 0);

            IOLog("NVDAAL-GSP: FWSEC StoredSize=0x%x fwOffset=0x%x\n",
                  fwsecInfo.storedSize, fwsecInfo.fwOffset);

            IOLog("NVDAAL-GSP: FWSEC extracted: IMEM=0x%x(%u) DMEM=0x%x(%u)\n",
                  fwsecInfo.imemOffset, fwsecInfo.imemSize,
                  fwsecInfo.dmemOffset, fwsecInfo.dmemSize);

            // Extract signature patching fields from FalconUcodeDescV3Nvidia
            // This structure is at the same offset as FalconUcodeDescV3 but has additional fields
            if (ucodeDescOffset + sizeof(FalconUcodeDescV3Nvidia) <= size) {
                const FalconUcodeDescV3Nvidia *ucodeNv = (const FalconUcodeDescV3Nvidia *)(data + ucodeDescOffset);

                // Validate - vDesc should be reasonable (usually 0x10000003 for v3)
                if ((ucodeNv->vDesc & 0x0000FFFF) == 3) {  // Version 3
                    fwsecInfo.pkcDataOffset = ucodeNv->pkcDataOffset;
                    fwsecInfo.interfaceOffset = ucodeNv->interfaceOffset;
                    fwsecInfo.ucodeId = ucodeNv->ucodeId;
                    fwsecInfo.signatureCount = ucodeNv->signatureCount;
                    fwsecInfo.signatureVersions = ucodeNv->signatureVersions;

                    // Signatures are stored immediately after the 44-byte header
                    fwsecInfo.signaturesOffset = ucodeDescOffset + FALCON_UCODE_DESC_V3_SIZE;
                    fwsecInfo.signaturesTotalSize = fwsecInfo.signatureCount * BCRT30_RSA3K_SIG_SIZE;

                    IOLog("NVDAAL-GSP: FWSEC Signature Info: ucodeId=%d, sigCount=%d, sigVersions=0x%04x\n",
                          fwsecInfo.ucodeId, fwsecInfo.signatureCount, fwsecInfo.signatureVersions);
                    IOLog("NVDAAL-GSP: FWSEC Signature Patching: pkcDataOff=0x%x, interfaceOff=0x%x\n",
                          fwsecInfo.pkcDataOffset, fwsecInfo.interfaceOffset);
                    IOLog("NVDAAL-GSP: FWSEC Signatures at offset 0x%x, totalSize=%d bytes\n",
                          fwsecInfo.signaturesOffset, fwsecInfo.signaturesTotalSize);
                } else {
                    IOLog("NVDAAL-GSP: vDesc=0x%08x - not v3 format, signature patching disabled\n",
                          ucodeNv->vDesc);
                    fwsecInfo.signatureCount = 0;
                    fwsecInfo.pkcDataOffset = 0;
                }
            } else {
                IOLog("NVDAAL-GSP: Descriptor too small for FalconUcodeDescV3Nvidia\n");
                fwsecInfo.signatureCount = 0;
                fwsecInfo.pkcDataOffset = 0;
            }

            // Find DMEMMAPPER in DMEM
            if (fwsecInfo.dmemOffset + fwsecInfo.dmemSize <= size) {
                const uint8_t *dmem = data + fwsecInfo.dmemOffset;
                for (uint32_t j = 0; j < fwsecInfo.dmemSize - 4; j += 4) {
                    if (*(uint32_t *)(dmem + j) == DMEMMAPPER_SIGNATURE) {
                        fwsecInfo.dmemMapperOffset = j;
                        IOLog("NVDAAL-GSP: Found DMEMMAPPER at DMEM+0x%x\n", j);
                        break;
                    }
                }
            }
            
            break;
        }
        
        entryOffset += pmuHdr->entrySize;
    }
    
    if (!fwsecInfo.valid) {
        IOLog("NVDAAL-GSP: Could not extract FWSEC ucode info\n");
    }
    
    return true;
}

bool NVDAALGsp::loadFalconUcode(uint32_t falconBase, const void *imem, size_t imemSize,
                                 const void *dmem, size_t dmemSize) {
    const uint32_t *imemData = (const uint32_t *)imem;
    const uint32_t *dmemData = (const uint32_t *)dmem;
    uint32_t numWords, wordIdx, tag;

    IOLog("NVDAAL-GSP: Loading Falcon ucode at 0x%x: IMEM=%lu DMEM=%lu\n",
          falconBase, (unsigned long)imemSize, (unsigned long)dmemSize);

    // Load IMEM with SECURE bit and IMEM tags (per NVIDIA s_imemCopyTo_TU102)
    // SECURE bit (28) marks code as HS-loadable, tags enable BROM hash verification
    numWords = (uint32_t)(imemSize / 4);
    tag = 0;

    // Set IMEMC: address=0, AINCW=1, SECURE=1
    writeReg(falconBase + FALCON_IMEMC(0), FALCON_IMEMC_AINCW | FALCON_IMEMC_SECURE);

    for (wordIdx = 0; wordIdx < numWords; wordIdx++) {
        // Write IMEM tag at every 256-byte (64-word) boundary
        if ((wordIdx & FALCON_IMEM_WORDS_PER_BLK) == 0) {
            writeReg(falconBase + FALCON_IMEMT(0), tag);
            tag++;
        }
        writeReg(falconBase + FALCON_IMEMD(0), imemData[wordIdx]);
    }

    IOLog("NVDAAL-GSP: IMEM loaded: %u words, %u tags\n", numWords, tag);

    // Load DMEM with auto-increment
    numWords = (uint32_t)(dmemSize / 4);
    writeReg(falconBase + FALCON_DMEMC(0), FALCON_DMEMC_AINCW);

    for (wordIdx = 0; wordIdx < numWords; wordIdx++) {
        writeReg(falconBase + FALCON_DMEMD(0), dmemData[wordIdx]);
    }

    IOLog("NVDAAL-GSP: DMEM loaded: %u words\n", numWords);
    return true;
}

// ============================================================================
// DMA-based Falcon Loading (for Heavy Secure mode)
// This allows the Boot ROM to verify the RSA-3K signature of FWSEC firmware
// ============================================================================

bool NVDAALGsp::loadFalconUcodeDma(uint32_t falconBase,
                                    IOBufferMemoryDescriptor *fwMem, uint64_t fwPhys,
                                    size_t fwSize, uint32_t bootVec) {
    IOLog("NVDAAL-GSP: Loading Falcon via DMA (GA102 HS path): phys=0x%llx size=%lu\n",
          fwPhys, (unsigned long)fwSize);

    // === Step 1: Engine reset (kflcnResetIntoRiscv_GA102) ===

    // PreResetWait: wait for HWCFG2 RESET_READY (bit 31)
    for (int i = 0; i < 150; i++) {
        uint32_t hwcfg2 = readReg(falconBase + FALCON_HWCFG2);
        if (hwcfg2 & FALCON_HWCFG2_RESET_READY) break;
        IODelay(1);
    }

    // Toggle ENGINE register (with register-read delays per NVIDIA)
    writeReg(falconBase + FALCON_ENGINE, 1);
    for (int i = 0; i < 10; i++) readReg(falconBase + FALCON_ENGINE);
    writeReg(falconBase + FALCON_ENGINE, 0);
    for (int i = 0; i < 10; i++) readReg(falconBase + FALCON_ENGINE);

    // Wait for scrub via HWCFG2 bit 12 (GA102 method)
    for (int i = 0; i < 50000; i++) {
        uint32_t hwcfg2 = readReg(falconBase + FALCON_HWCFG2);
        if (!(hwcfg2 & FALCON_HWCFG2_MEM_SCRUBBING)) break;
        IODelay(1);
    }

    // === Step 2: Switch to Falcon core (kflcnSwitchToFalcon_GA102) ===
    // Write BCR_CTRL: CORE_SELECT=FALCON (0), wait for VALID bit
    writeReg(falconBase + FALCON_BCR_CTRL, 0);
    for (int i = 0; i < 10000; i++) {
        IODelay(1);
        if (readReg(falconBase + FALCON_BCR_CTRL) & FALCON_BCR_CTRL_VALID) break;
    }

    // Set RM register to PMC_BOOT_0
    uint32_t boot0 = readReg(NV_PMC_BOOT_0);
    writeReg(falconBase + FALCON_RM, boot0);

    // === Step 3: Disable context requirement (kflcnDisableCtxReq_TU102) ===
    uint32_t fbifCtl = readReg(falconBase + FALCON_FBIF_CTL);
    fbifCtl |= FALCON_FBIF_CTL_ALLOW_PHYS_NO_CTX;
    writeReg(falconBase + FALCON_FBIF_CTL, fbifCtl);
    writeReg(falconBase + FALCON_DMACTL, 0);

    // === Step 4: Configure TRANSCFG for coherent sysmem + physical ===
    uint32_t transcfg = readReg(falconBase + FALCON_FBIF_TRANSCFG(0));
    transcfg &= ~0x07;
    transcfg |= FALCON_TRANSCFG_TARGET_COHERENT | (1 << 2);  // COHERENT + PHYSICAL
    writeReg(falconBase + FALCON_FBIF_TRANSCFG(0), transcfg);

    // === Step 5: Set DMA base address ===
    writeReg(falconBase + FALCON_DMATRFBASE, (uint32_t)(fwPhys >> 8));
    writeReg(falconBase + FALCON_DMATRFBASE1, (uint32_t)(fwPhys >> 40));
    IOLog("NVDAAL-GSP: DMA base: 0x%08x (hi: 0x%08x)\n",
          (uint32_t)(fwPhys >> 8), (uint32_t)(fwPhys >> 40));

    // === Step 6: DMA load IMEM with SEC=1 (kgspExecuteHsFalcon_GA102) ===
    // SEC bit marks IMEM as secure for BROM hash verification
    IOLog("NVDAAL-GSP: DMA loading IMEM (%lu bytes)...\n", (unsigned long)fwSize);

    for (size_t offset = 0; offset < fwSize; offset += 256) {
        writeReg(falconBase + FALCON_DMATRFMOFFS, (uint32_t)offset);
        writeReg(falconBase + FALCON_DMATRFFBOFFS, (uint32_t)offset);

        // IMEM: SEC=1 (bit 2) + IMEM target (bit 4) + SIZE_256B (6 << 8)
        uint32_t cmd = FALCON_DMA_CMD_IMEM | FALCON_DMATRFCMD_SEC_IMEM | (6 << 8);
        writeReg(falconBase + FALCON_DMATRFCMD, cmd);

        for (int wait = 0; wait < 2000; wait++) {
            if (readReg(falconBase + FALCON_DMATRFCMD) & FALCON_DMA_CMD_IDLE) break;
            IODelay(1);
        }
    }

    IOLog("NVDAAL-GSP: DMA IMEM loaded\n");
    return true;
}

bool NVDAALGsp::executeFwsecViaBrom(void) {
    // Execute FWSEC via BROM (Boot ROM) in Heavy-Secure mode
    // Based on NVIDIA kgspExecuteHsFalcon_GA102
    // Flow: reset → DisableCtxReq → TRANSCFG → DMA load → BROM params → start

    IOLog("NVDAAL-GSP: Executing FWSEC via BROM on GSP Falcon (Ada HS DMA path)...\n");

    if (!fwsecMem || !fwsecInfo.valid) {
        IOLog("NVDAAL-GSP: No valid FWSEC firmware loaded\n");
        return false;
    }

    // CRITICAL: FWSEC runs on GSP Falcon (0x110000) in Heavy-Secure mode
    // Confirmed via nouveau fwsec.c, open-gpu-kernel-modules kernel_gsp_fwsec.c,
    // and nova-core docs (docs.kernel.org/gpu/nova/core/fwsec.html)
    // NOTE: Booter load/unload runs on SEC2 (0x840000), but FWSEC is on GSP
    const uint32_t falconBase = NV_PGSP_BASE;
    const uint8_t *vbiosData = (const uint8_t *)fwsecMem->getBytesNoCopy();

    // Prepare DMA buffer with IMEM + DMEM (256-byte aligned)
    size_t imemAligned = (fwsecInfo.imemSize + 255) & ~255;
    size_t dmemAligned = (fwsecInfo.dmemSize + 255) & ~255;
    size_t dmaSize = imemAligned + dmemAligned;

    IOBufferMemoryDescriptor *dmaBuf = nullptr;
    uint64_t dmaPhys = 0;

    if (!allocDmaBuffer(&dmaBuf, dmaSize, &dmaPhys)) {
        IOLog("NVDAAL-GSP: Failed to allocate FWSEC DMA buffer (%lu bytes)\n",
              (unsigned long)dmaSize);
        return false;
    }

    uint8_t *dmaVirt = (uint8_t *)dmaBuf->getBytesNoCopy();
    memset(dmaVirt, 0, dmaSize);

    // Copy IMEM at offset 0
    const void *imem = vbiosData + fwsecInfo.imemOffset;
    memcpy(dmaVirt, imem, fwsecInfo.imemSize);

    // Copy patched DMEM at imemAligned offset
    // We need a working copy of DMEM for signature + FRTS patching
    uint8_t *dmemCopy = (uint8_t *)IOMalloc(fwsecInfo.dmemSize);
    if (!dmemCopy) {
        freeDmaBuffer(&dmaBuf);
        return false;
    }
    memcpy(dmemCopy, vbiosData + fwsecInfo.dmemOffset, fwsecInfo.dmemSize);

    // Patch RSA signature
    if (fwsecInfo.signatureCount > 0 && fwsecInfo.pkcDataOffset > 0) {
        patchFwsecSignature(dmemCopy, fwsecInfo.dmemSize);
    }

    // Patch FRTS command
    uint64_t frtsOffset = 0;  // FWSEC computes from FB size
    patchFrtsCmdBuffer(dmemCopy, fwsecInfo.dmemSize, frtsOffset);

    memcpy(dmaVirt + imemAligned, dmemCopy, fwsecInfo.dmemSize);
    IOFree(dmemCopy, fwsecInfo.dmemSize);

    // Flush DMA buffer
    OSSynchronizeIO();

    IOLog("NVDAAL-GSP: DMA buffer @ 0x%llx: IMEM=%lu DMEM=%lu\n",
          dmaPhys, (unsigned long)fwsecInfo.imemSize, (unsigned long)fwsecInfo.dmemSize);

    // === Engine reset (GA102 sequence) ===
    // PreResetWait
    for (int i = 0; i < 150; i++) {
        if (readReg(falconBase + FALCON_HWCFG2) & FALCON_HWCFG2_RESET_READY) break;
        IODelay(1);
    }

    // Toggle ENGINE with register-read delays
    writeReg(falconBase + FALCON_ENGINE, 1);
    for (int i = 0; i < 10; i++) readReg(falconBase + FALCON_ENGINE);
    writeReg(falconBase + FALCON_ENGINE, 0);
    for (int i = 0; i < 10; i++) readReg(falconBase + FALCON_ENGINE);

    // Wait for scrub (HWCFG2 bit 12)
    for (int i = 0; i < 50000; i++) {
        if (!(readReg(falconBase + FALCON_HWCFG2) & FALCON_HWCFG2_MEM_SCRUBBING)) break;
        IODelay(1);
    }

    // Switch to Falcon core
    writeReg(falconBase + FALCON_BCR_CTRL, 0);
    for (int i = 0; i < 10000; i++) {
        IODelay(1);
        if (readReg(falconBase + FALCON_BCR_CTRL) & FALCON_BCR_CTRL_VALID) break;
    }

    // Set RM register
    writeReg(falconBase + FALCON_RM, readReg(NV_PMC_BOOT_0));

    // === DisableCtxReq ===
    uint32_t fbifCtl = readReg(falconBase + FALCON_FBIF_CTL);
    fbifCtl |= FALCON_FBIF_CTL_ALLOW_PHYS_NO_CTX;
    writeReg(falconBase + FALCON_FBIF_CTL, fbifCtl);
    writeReg(falconBase + FALCON_DMACTL, 0);

    // === Configure TRANSCFG ===
    uint32_t transcfg = readReg(falconBase + FALCON_FBIF_TRANSCFG(0));
    transcfg &= ~0x07;
    transcfg |= FALCON_TRANSCFG_TARGET_COHERENT | (1 << 2);  // COHERENT + PHYSICAL
    writeReg(falconBase + FALCON_FBIF_TRANSCFG(0), transcfg);

    // === Set DMA base ===
    writeReg(falconBase + FALCON_DMATRFBASE, (uint32_t)(dmaPhys >> 8));
    writeReg(falconBase + FALCON_DMATRFBASE1, (uint32_t)(dmaPhys >> 40));

    // === DMA load IMEM with SEC=1 ===
    IOLog("NVDAAL-GSP: DMA loading IMEM (%lu bytes)...\n", (unsigned long)imemAligned);
    for (size_t offset = 0; offset < imemAligned; offset += 256) {
        writeReg(falconBase + FALCON_DMATRFMOFFS, (uint32_t)offset);
        writeReg(falconBase + FALCON_DMATRFFBOFFS, (uint32_t)offset);
        uint32_t cmd = FALCON_DMA_CMD_IMEM | FALCON_DMATRFCMD_SEC_IMEM | (6 << 8);
        writeReg(falconBase + FALCON_DMATRFCMD, cmd);

        for (int wait = 0; wait < 2000; wait++) {
            if (readReg(falconBase + FALCON_DMATRFCMD) & FALCON_DMA_CMD_IDLE) break;
            IODelay(1);
        }
    }

    // === DMA load DMEM (SEC=0) ===
    IOLog("NVDAAL-GSP: DMA loading DMEM (%lu bytes)...\n", (unsigned long)dmemAligned);
    for (size_t offset = 0; offset < dmemAligned; offset += 256) {
        writeReg(falconBase + FALCON_DMATRFMOFFS, (uint32_t)offset);
        writeReg(falconBase + FALCON_DMATRFFBOFFS, (uint32_t)(imemAligned + offset));
        uint32_t cmd = FALCON_DMA_CMD_DMEM | (6 << 8);  // DMEM, no SEC
        writeReg(falconBase + FALCON_DMATRFCMD, cmd);

        for (int wait = 0; wait < 2000; wait++) {
            if (readReg(falconBase + FALCON_DMATRFCMD) & FALCON_DMA_CMD_IDLE) break;
            IODelay(1);
        }
    }

    IOLog("NVDAAL-GSP: DMA load complete\n");

    // === Set BROM parameters (via RISCV register space) ===
    IOLog("NVDAAL-GSP: Setting BROM parameters: ucodeId=%u engIdMask=0x%x\n",
          fwsecInfo.ucodeId, fwsecInfo.signatureVersions);

    writeReg(falconBase + FALCON_BROM_PARAADDR, fwsecInfo.pkcDataOffset);
    writeReg(falconBase + FALCON_BROM_ENGIDMASK, fwsecInfo.signatureVersions);  // Engine ID mask from V3 desc
    writeReg(falconBase + FALCON_BROM_CURR_UCODE_ID, fwsecInfo.ucodeId);
    writeReg(falconBase + FALCON_BROM_MOD_SEL, 1);  // RSA-3K

    // Set boot vector
    writeReg(falconBase + FALCON_BOOTVEC, fwsecInfo.bootVec);

    // === Start via CPUCTL_ALIAS (triggers BROM HS verification) ===
    IOLog("NVDAAL-GSP: Starting Falcon in HS mode via CPUCTL_ALIAS...\n");

    // Read CPUCTL diagnostic before start
    uint32_t cpuctlBefore = readReg(falconBase + FALCON_CPUCTL);
    IOLog("NVDAAL-GSP: CPUCTL before: 0x%08x (halted=%u alias_en=%u)\n",
          cpuctlBefore, (cpuctlBefore >> 4) & 1, (cpuctlBefore >> 6) & 1);

    writeReg(falconBase + FALCON_CPUCTL_ALIAS, FALCON_CPUCTL_STARTCPU);
    IODelay(100);

    uint32_t cpuctlAfter = readReg(falconBase + FALCON_CPUCTL);
    IOLog("NVDAAL-GSP: CPUCTL after: 0x%08x (halted=%u alias_en=%u)\n",
          cpuctlAfter, (cpuctlAfter >> 4) & 1, (cpuctlAfter >> 6) & 1);

    // === Wait for completion ===
    IOLog("NVDAAL-GSP: Waiting for FWSEC HS execution...\n");
    bool halted = false;
    for (int i = 0; i < 5000; i++) {
        uint32_t cpuctl = readReg(falconBase + FALCON_CPUCTL);
        if (cpuctl & FALCON_CPUCTL_HALTED) {
            uint32_t mbox0 = readReg(falconBase + FALCON_MAILBOX0);
            uint32_t scratch0e = readReg(NV_PBUS_SW_SCRATCH_0E);
            IOLog("NVDAAL-GSP: Falcon halted: CPUCTL=0x%08x MBOX0=0x%08x Scratch0E=0x%08x\n",
                  cpuctl, mbox0, scratch0e);

            if (mbox0 != 0) {
                IOLog("NVDAAL-GSP: FWSEC BROM error in mailbox: 0x%08x\n", mbox0);
            }
            halted = true;
            break;
        }
        if (i == 100 || i == 1000 || i == 3000) {
            IOLog("NVDAAL-GSP: Still waiting... cpuctl=0x%08x\n", cpuctl);
        }
        IODelay(1000);
    }

    freeDmaBuffer(&dmaBuf);

    if (!halted) {
        IOLog("NVDAAL-GSP: FWSEC HS execution timeout\n");
        return false;
    }

    // Check FRTS status register (0x001438 = NV_PBUS_VBIOS_SCRATCH + 0x0E*4)
    uint32_t frtsStatus = readReg(NV_PBUS_VBIOS_SCRATCH_FRTS_ERR);
    uint32_t frtsErr = (frtsStatus >> 16) & 0xFFFF;
    IOLog("NVDAAL-GSP: FRTS status reg: 0x%08x (error=%u)\n", frtsStatus, frtsErr);
    if (frtsErr != 0) {
        IOLog("NVDAAL-GSP: FWSEC-FRTS reported error: %u\n", frtsErr);
    }

    // Check WPR2
    if (checkWpr2Setup()) {
        IOLog("NVDAAL-GSP: WPR2 configured via BROM HS execution!\n");
        return true;
    }

    IOLog("NVDAAL-GSP: BROM execution completed but WPR2 not configured\n");
    return false;
}

bool NVDAALGsp::parseElfFirmware(const void *data, size_t size) {
    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)data;
    const uint8_t *bytes = (const uint8_t *)data;

    // Check ELF magic: 0x7F 'E' 'L' 'F'
    if (ehdr->ident[0] != 0x7F || ehdr->ident[1] != 'E' || 
        ehdr->ident[2] != 'L' || ehdr->ident[3] != 'F') {
        IOLog("NVDAAL-GSP: Invalid ELF magic\n");
        return false;
    }

    // Check 64-bit class (2)
    if (ehdr->ident[4] != 2) {
        IOLog("NVDAAL-GSP: Not a 64-bit ELF\n");
        return false;
    }

    // Validate size
    if (size < sizeof(Elf64_Ehdr)) {
        IOLog("NVDAAL-GSP: Firmware too small for header\n");
        return false;
    }

    // Get section header string table
    if (ehdr->shoff + (ehdr->shnum * ehdr->shentsize) > size) {
        IOLog("NVDAAL-GSP: Section headers invalid\n");
        return false;
    }

    const Elf64_Shdr *shdrs = (const Elf64_Shdr *)(bytes + ehdr->shoff);
    const Elf64_Shdr *shstrtab = &shdrs[ehdr->shstrndx];
    const char *strs = (const char *)(bytes + shstrtab->offset);

    IOLog("NVDAAL-GSP: Parsing ELF (%d sections)...\n", ehdr->shnum);

    // Reset offsets
    firmwareCodeOffset = 0;
    firmwareDataOffset = 0;
    firmwareSize = 0;

    for (int i = 0; i < ehdr->shnum; i++) {
        const Elf64_Shdr *shdr = &shdrs[i];
        const char *name = strs + shdr->name;

        // Found .fwimage section?
        if (strcmp(name, GSP_FW_SECTION_IMAGE) == 0) {
            IOLog("NVDAAL-GSP: Found .fwimage: offset 0x%llx, size 0x%llx\n",
                  shdr->offset, shdr->size);

            firmwareCodeOffset = shdr->offset;
            firmwareSize = shdr->size;

            // Allocate firmware memory (non-contiguous, DMA-able)
            // NOTE: For large firmware (63MB), we can't use physically contiguous
            firmwareMem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
                kernel_task,
                kIODirectionInOut,  // No kIOMemoryPhysicallyContiguous for large allocs
                firmwareSize,
                0xFFFFFFFFFFFFULL   // 48-bit physical address mask
            );

            if (!firmwareMem) {
                IOLog("NVDAAL-GSP: Failed to allocate firmware memory (%llu bytes)\n",
                      (unsigned long long)firmwareSize);
                return false;
            }

            IOReturn ret = firmwareMem->prepare();
            if (ret != kIOReturnSuccess) {
                IOLog("NVDAAL-GSP: Failed to prepare firmware memory (0x%x)\n", ret);
                firmwareMem->release();
                firmwareMem = nullptr;
                return false;
            }

            // Copy firmware data
            memcpy(firmwareMem->getBytesNoCopy(), bytes + shdr->offset, shdr->size);

            // Build the page table for this firmware (handles non-contiguous pages)
            if (!buildRadix3PageTable(firmwareMem->getBytesNoCopy(), firmwareSize)) {
                return false;
            }
        }
        // TODO: Handle signature sections if needed for verified boot
        else if (strcmp(name, GSP_FW_SECTION_SIG_AD10X) == 0) {
             IOLog("NVDAAL-GSP: Found signature AD10X (skipping for now)\n");
        }
    }

    if (firmwareSize == 0) {
        IOLog("NVDAAL-GSP: .fwimage section not found in ELF\n");
        return false;
    }

    return true;
}

bool NVDAALGsp::buildRadix3PageTable(const void *firmware, size_t size) {
    // Radix3 is a 64-bit sparse page table format
    // Each entry is 64-bit (8 bytes)
    // Page size is 4KB (0x1000)
    
    // We need to map 'size' bytes of firmware.
    // Assuming virtual address starts at 0 for simplicity (or whatever GSP expects)
    
    // Level 0 (Root): 1 page, covers 512 Level 1 entries
    // Level 1: Covers 512 Level 2 entries
    // Level 2: Covers 512 Data Pages (Level 3)
    
    // For GSP, it seems we just need a linear mapping of the firmware blob.
    
    uint64_t numPages = (size + GSP_PAGE_SIZE - 1) / GSP_PAGE_SIZE;
    
    // Calculate required pages for the table itself
    // We need 1 root page.
    // Number of L2 tables (leaf tables) needed:
    uint64_t numL2Tables = (numPages + 511) / 512;
    // Number of L1 tables needed to cover L2 tables:
    uint64_t numL1Tables = (numL2Tables + 511) / 512;
    
    // Total allocation size for page tables (excluding data)
    // Root + L1s + L2s
    size_t tableSize = (1 + numL1Tables + numL2Tables) * GSP_PAGE_SIZE;
    
    if (!allocDmaBuffer(&radix3Mem, tableSize, &radix3Phys)) {
        IOLog("NVDAAL-GSP: Failed to allocate Radix3 tables\n");
        return false;
    }
    
    uint64_t *tableBase = (uint64_t *)radix3Mem->getBytesNoCopy();
    memset(tableBase, 0, tableSize);
    
    // Physical address of the table buffer
    uint64_t tableBasePhys = radix3Phys;
    
    // Pointers to current tables being filled
    uint64_t *rootTable = tableBase;
    uint64_t *l1Table = rootTable + 512; // Next 4KB
    uint64_t *l2Table = l1Table + (numL1Tables * 512); // After all L1s
    
    // Physical addresses corresponding to pointers
    uint64_t l1Phys = tableBasePhys + GSP_PAGE_SIZE;
    uint64_t l2Phys = l1Phys + (numL1Tables * GSP_PAGE_SIZE);
    
    // Fill Root Table (L0)
    for (uint64_t i = 0; i < numL1Tables; i++) {
        // Entry format: Physical Address >> 12 (PFN)
        // Or is it full address? Usually PFN | Valid bit.
        // TinyGPU uses: (addr) | 1 (Valid)
        // But let's check standard GSP behavior.
        // NVIDIA drivers usually use full address for GSP radix3.
        
        rootTable[i] = (l1Phys + (i * GSP_PAGE_SIZE)) | 1; // Mark Valid
    }
    
    // Fill L1 Tables
    for (uint64_t i = 0; i < numL2Tables; i++) {
        l1Table[i] = (l2Phys + (i * GSP_PAGE_SIZE)) | 1; // Mark Valid
    }
    
    // Fill L2 Tables (Leafs) - Point to Firmware Data Pages
    // NOTE: Firmware memory may not be physically contiguous, so we must
    // get the physical address of each page individually from the descriptor
    for (uint64_t i = 0; i < numPages; i++) {
        IOByteCount segLen;
        uint64_t pagePhys = firmwareMem->getPhysicalSegment(i * GSP_PAGE_SIZE, &segLen);
        if (pagePhys == 0) {
            IOLog("NVDAAL-GSP: Failed to get physical address for page %llu\n", i);
            return false;
        }
        l2Table[i] = pagePhys | 1; // Mark Valid
    }

    IOLog("NVDAAL-GSP: Radix3 built. Root: 0x%llx, Pages: %llu, TableSize: %lu bytes\n",
          radix3Phys, numPages, tableSize);
    
    return true;
}

// ============================================================================
// WPR Metadata Setup
// ============================================================================

bool NVDAALGsp::setupWprMeta(void) {
    if (!wprMetaMem) {
        return false;
    }

    GspFwWprMeta *meta = (GspFwWprMeta *)wprMetaMem->getBytesNoCopy();
    memset(meta, 0, sizeof(GspFwWprMeta));

    meta->magic = 0x57505232;  // "WPR2"

    // Bootloader info (The small secure booter ucode)
    meta->sysmemAddrOfBootloader = bootloaderPhys;
    meta->sizeOfBootloader = bootloaderMem ? bootloaderMem->getLength() : 0;

    // Radix3 Page Table (Maps the large GSP firmware)
    meta->sysmemAddrOfRadix3Elf = radix3Phys;
    meta->sizeOfRadix3Elf = radix3Mem ? radix3Mem->getLength() : 0;

    // Memory Regions
    meta->gspFwHeapSize = GSP_HEAP_SIZE;
    meta->frtsSize = FRTS_SIZE;
    
    // VGPU/Compute specifics
    meta->fwHeapEnabled = 1;
    meta->partitionRpc = 1;

    // Offsets within the firmware image if needed
    // In Ada, these are often zero if the whole image is mapped via Radix3
    meta->bootBinVirtAddr = 0; 
    meta->gspFwOffset = 0;

    IOLog("NVDAAL-GSP: WPR metadata configured at 0x%llx\n", wprMetaPhys);
    IOLog("NVDAAL-GSP:   Bootloader: 0x%llx (%llu bytes)\n", 
          meta->sysmemAddrOfBootloader, meta->sizeOfBootloader);
    IOLog("NVDAAL-GSP:   Radix3:     0x%llx (%llu bytes)\n", 
          meta->sysmemAddrOfRadix3Elf, meta->sizeOfRadix3Elf);

    return true;
}

// ============================================================================
// Boot Sequence
// ============================================================================

// Returns boot stage for debugging (0=success, negative=error at stage)
int NVDAALGsp::bootEx(void) {
    if (!initialized) {
        IOLog("NVDAAL-GSP: Not initialized\n");
        return -1;
    }

    IOLog("NVDAAL-GSP: Starting boot sequence (Ada Lovelace)...\n");

    // Read current GSP state
    uint32_t riscvCtl = readReg(NV_PRISCV_RISCV_CPUCTL);
    uint32_t falconCtl = readReg(NV_PGSP_FALCON_CPUCTL);
    uint32_t wpr2Hi = readReg(NV_PFB_PRI_MMU_WPR2_ADDR_HI);
    IOLog("NVDAAL-GSP: Pre-boot state: RISCV_CTL=0x%08x FALCON_CTL=0x%08x WPR2_HI=0x%08x\n",
          riscvCtl, falconCtl, wpr2Hi);

    // Check if WPR2 is already set up (by EFI/previous driver)
    if (NV_PFB_WPR2_ENABLED(wpr2Hi)) {
        IOLog("NVDAAL-GSP: WPR2 already active - need GPU reset first\n");
        // For now, we'll try to continue - may need PCI reset
    }

    // Step 1: Reset both FALCON and SEC2
    IOLog("NVDAAL-GSP: Step 1 - Reset GSP FALCON\n");
    if (!resetFalcon()) {
        IOLog("NVDAAL-GSP: FALCON reset failed\n");
        return -2;
    }

    IOLog("NVDAAL-GSP: Step 1b - Reset SEC2\n");
    if (!resetSec2()) {
        IOLog("NVDAAL-GSP: SEC2 reset failed (continuing anyway)\n");
        // Non-fatal for now
    }

    // Step 2: Execute FWSEC-FRTS to set up WPR2
    // First, try to read VBIOS from BAR0 if not already loaded
    if (!fwsecMem) {
        IOLog("NVDAAL-GSP: Step 2a - Reading VBIOS from BAR0...\n");
        if (!readVbiosFromBar()) {
            IOLog("NVDAAL-GSP: Failed to read VBIOS from BAR0\n");
        }
    }

    if (fwsecMem) {
        IOLog("NVDAAL-GSP: Step 2b - Execute FWSEC-FRTS\n");
        if (!executeFwsecFrts()) {
            IOLog("NVDAAL-GSP: FWSEC-FRTS failed - continuing in debug mode\n");
            // Don't return error - try to continue anyway
        }
    } else {
        IOLog("NVDAAL-GSP: Step 2b - No VBIOS available, checking WPR2 status\n");
        if (!checkWpr2Setup()) {
            IOLog("NVDAAL-GSP: WPR2 not set up - continuing in debug mode\n");
        }
    }

    // Step 3: Setup WPR metadata
    IOLog("NVDAAL-GSP: Step 3 - Setup WPR metadata\n");
    if (!setupWprMeta()) {
        IOLog("NVDAAL-GSP: WPR meta setup failed\n");
        return -4;
    }

    // Step 4: Execute booter_load on SEC2 (if available)
    if (booterLoadMem) {
        IOLog("NVDAAL-GSP: Step 4 - Execute booter_load on SEC2\n");
        if (!executeBooterLoad()) {
            IOLog("NVDAAL-GSP: booter_load execution failed - trying direct start\n");
            // Don't return error - try direct RISC-V start
        }
    } else {
        IOLog("NVDAAL-GSP: Step 4 - No booter_load, trying direct RISC-V start\n");
    }

    // Step 5: Start RISC-V core
    IOLog("NVDAAL-GSP: Step 5 - Start RISC-V core\n");
    if (!startRiscv()) {
        IOLog("NVDAAL-GSP: RISC-V start failed\n");
        // Read post-boot state for diagnostics
        uint32_t retcode = readReg(NV_PRISCV_RISCV_BR_RETCODE);
        riscvCtl = readReg(NV_PRISCV_RISCV_CPUCTL);
        uint32_t scratch14 = readReg(NV_PGC6_BSI_SECURE_SCRATCH_14);
        IOLog("NVDAAL-GSP: Post-boot: RISCV_CTL=0x%08x BR_RETCODE=0x%08x SCRATCH14=0x%08x\n",
              riscvCtl, retcode, scratch14);
        return -6;
    }

    IOLog("NVDAAL-GSP: Boot sequence initiated, waiting for init...\n");
    return 0;
}

bool NVDAALGsp::boot(void) {
    return bootEx() == 0;
}

bool NVDAALGsp::resetFalcon(void) {
    // Reset the FALCON/GSP processor
    IOLog("NVDAAL-GSP: Resetting FALCON...\n");

    // Write reset sequence
    writeReg(NV_PGSP_FALCON_CPUCTL, 0);
    IODelay(100);  // 100us delay

    // Check if halted
    uint32_t cpuctl = readReg(NV_PGSP_FALCON_CPUCTL);
    if (!(cpuctl & FALCON_CPUCTL_HALTED)) {
        IOLog("NVDAAL-GSP: FALCON not halted after reset\n");
        // Continue anyway for now
    }

    return true;
}

bool NVDAALGsp::resetSec2(void) {
    // Reset SEC2 RISC-V (Ada Lovelace uses RISC-V, not classic Falcon)
    IOLog("NVDAAL-GSP: Resetting SEC2 RISC-V...\n");

    // For RISC-V cores, we need to use the RISCV registers
    uint32_t cpuctl = readReg(NV_PSEC_RISCV_CPUCTL);
    IOLog("NVDAAL-GSP: SEC2 RISCV_CPUCTL before reset: 0x%08x\n", cpuctl);
    
    // Try Falcon reset first (some hybrid cores need this)
    writeReg(NV_PSEC_FALCON_CPUCTL, 0);
    IODelay(100);
    
    // Check RISC-V status
    cpuctl = readReg(NV_PSEC_RISCV_CPUCTL);
    IOLog("NVDAAL-GSP: SEC2 RISCV_CPUCTL after reset: 0x%08x\n", cpuctl);
    
    // RISC-V CPUCTL bit 4 = halted
    if (!(cpuctl & 0x10)) {
        IOLog("NVDAAL-GSP: SEC2 RISC-V not halted, trying RISC-V halt\n");
        // Try to halt via RISC-V control
        writeReg(NV_PSEC_RISCV_CPUCTL, cpuctl | 0x10);
        IODelay(100);
        cpuctl = readReg(NV_PSEC_RISCV_CPUCTL);
        IOLog("NVDAAL-GSP: SEC2 RISCV_CPUCTL after halt: 0x%08x\n", cpuctl);
    }

    return (cpuctl & 0x10) != 0;  // Return true if halted
}

bool NVDAALGsp::checkWpr2Setup(void) {
    // Check if WPR2 has been set up by FWSEC
    // Ada Lovelace: LO=0x1FA824, HI=0x1FA828 (confirmed via nvlddmkm.sys 591.74)
    uint32_t wpr2LoReg = readReg(NV_PFB_PRI_MMU_WPR2_ADDR_LO);
    uint32_t wpr2HiReg = readReg(NV_PFB_PRI_MMU_WPR2_ADDR_HI);

    IOLog("NVDAAL-GSP: WPR2 raw: LO=0x%08x HI=0x%08x\n", wpr2LoReg, wpr2HiReg);

    if (NV_PFB_WPR2_ENABLED(wpr2HiReg)) {
        // WPR2 registers contain page-aligned addresses (shifted by 12 bits)
        wpr2Lo = ((uint64_t)(wpr2LoReg & 0xFFFFF)) << 12;
        wpr2Hi = ((uint64_t)(wpr2HiReg & 0xFFFFF)) << 12;

        IOLog("NVDAAL-GSP: WPR2 active: 0x%llx - 0x%llx\n", wpr2Lo, wpr2Hi);
        return true;
    }

    IOLog("NVDAAL-GSP: WPR2 not active\n");
    return false;
}

uint64_t NVDAALGsp::getWpr2Lo(void) {
    return wpr2Lo;
}

uint64_t NVDAALGsp::getWpr2Hi(void) {
    return wpr2Hi;
}

// ============================================================================
// Fuse Version and Signature Helpers (from NVIDIA open-gpu-kernel-modules)
// ============================================================================

uint32_t NVDAALGsp::readUcodeFuseVersion(uint8_t ucodeId) {
    // Read the fuse version for a given ucode ID
    // Based on kgspReadUcodeFuseVersion_GA100 from NVIDIA driver

    if (ucodeId == 0 || ucodeId > 16) {
        IOLog("NVDAAL-GSP: Invalid ucodeId %d (must be 1-16)\n", ucodeId);
        return 0;
    }

    uint32_t index = ucodeId - 1;  // Convert to 0-indexed
    uint32_t fuseReg = NV_FUSE_OPT_FPF_GSP_UCODE1_VERSION + (4 * index);
    uint32_t fuseVal = readReg(fuseReg);

    IOLog("NVDAAL-GSP: Fuse reg 0x%08x (ucodeId=%d) = 0x%08x\n", fuseReg, ucodeId, fuseVal);

    if (fuseVal == 0) {
        return 0;
    }

    // Find highest bit set and return its index + 1
    // HIGHESTBITIDX_32 equivalent
    uint32_t version = 0;
    uint32_t temp = fuseVal;
    while (temp >>= 1) {
        version++;
    }

    IOLog("NVDAAL-GSP: Fuse version for ucodeId %d = %d\n", ucodeId, version + 1);
    return version + 1;
}

bool NVDAALGsp::patchFwsecSignature(uint8_t *dmem, size_t dmemSize) {
    // Patch the RSA-3K signature into DMEM at pkcDataOffset
    // Based on s_prepareForFwsec_TU102 from NVIDIA driver

    if (!fwsecInfo.valid) {
        IOLog("NVDAAL-GSP: FWSEC info not valid for signature patching\n");
        return false;
    }

    if (fwsecInfo.signatureCount == 0 || fwsecInfo.signatureVersions == 0) {
        IOLog("NVDAAL-GSP: No signatures available in FWSEC (count=%d, versions=0x%04x)\n",
              fwsecInfo.signatureCount, fwsecInfo.signatureVersions);
        return false;
    }

    // Read fuse version for this ucode
    uint32_t fuseVersion = readUcodeFuseVersion(fwsecInfo.ucodeId);
    if (fuseVersion == 0) {
        IOLog("NVDAAL-GSP: Fuse version is 0, trying version 1 as default\n");
        fuseVersion = 1;
    }

    // Convert fuse version to bit mask
    uint32_t ucodeVersionBit = 1 << (fuseVersion - 1);
    uint32_t sigVersions = fwsecInfo.signatureVersions;

    IOLog("NVDAAL-GSP: Signature selection: fuseVer=%d, versionBit=0x%08x, sigVersions=0x%04x\n",
          fuseVersion, ucodeVersionBit, sigVersions);

    // Check if this fuse version has a signature
    if ((ucodeVersionBit & sigVersions) == 0) {
        IOLog("NVDAAL-GSP: ERROR: No signature for fuse version %d (sigVersions=0x%04x)\n",
              fuseVersion, sigVersions);
        return false;
    }

    // Calculate signature offset by counting set bits before our version
    uint32_t sigOffset = 0;
    uint32_t tempVersionBit = ucodeVersionBit;
    uint32_t tempSigVersions = sigVersions;

    while ((tempVersionBit & tempSigVersions & 1) == 0) {
        sigOffset += (tempSigVersions & 1) * BCRT30_RSA3K_SIG_SIZE;
        tempSigVersions >>= 1;
        tempVersionBit >>= 1;
    }

    IOLog("NVDAAL-GSP: Selected signature at offset %d (index %d)\n",
          sigOffset, sigOffset / BCRT30_RSA3K_SIG_SIZE);

    // Validate offsets
    if (fwsecInfo.pkcDataOffset + BCRT30_RSA3K_SIG_SIZE > dmemSize) {
        IOLog("NVDAAL-GSP: ERROR: pkcDataOffset 0x%x + sigSize %d exceeds DMEM size %lu\n",
              fwsecInfo.pkcDataOffset, BCRT30_RSA3K_SIG_SIZE, (unsigned long)dmemSize);
        return false;
    }

    if (sigOffset + BCRT30_RSA3K_SIG_SIZE > fwsecInfo.signaturesTotalSize) {
        IOLog("NVDAAL-GSP: ERROR: sigOffset %d + sigSize exceeds total signatures size %d\n",
              sigOffset, fwsecInfo.signaturesTotalSize);
        return false;
    }

    // Get pointer to signature data in VBIOS
    const uint8_t *vbiosData = (const uint8_t *)fwsecMem->getBytesNoCopy();
    const uint8_t *sigData = vbiosData + fwsecInfo.signaturesOffset + sigOffset;

    // Patch signature into DMEM
    IOLog("NVDAAL-GSP: Patching signature (%d bytes) at DMEM+0x%x\n",
          BCRT30_RSA3K_SIG_SIZE, fwsecInfo.pkcDataOffset);
    memcpy(dmem + fwsecInfo.pkcDataOffset, sigData, BCRT30_RSA3K_SIG_SIZE);

    IOLog("NVDAAL-GSP: Signature patched successfully\n");
    return true;
}

bool NVDAALGsp::patchFrtsCmdBuffer(uint8_t *dmem, size_t dmemSize, uint64_t frtsOffset) {
    // Patch the FRTS command buffer in DMEM
    // Based on s_prepareForFwsec_TU102 from NVIDIA driver

    if (!fwsecInfo.valid) {
        IOLog("NVDAAL-GSP: FWSEC info not valid for FRTS cmd patching\n");
        return false;
    }

    // Find DMEMMAPPER in DMEM
    DmemMapperHeader *mapper = nullptr;
    if (fwsecInfo.dmemMapperOffset > 0 &&
        fwsecInfo.dmemMapperOffset + sizeof(DmemMapperHeader) <= dmemSize) {
        mapper = (DmemMapperHeader *)(dmem + fwsecInfo.dmemMapperOffset);
    } else if (fwsecInfo.interfaceOffset > 0) {
        // Try using interfaceOffset from DescV3Nvidia
        // Interface header is: version(1), headerSize(1), entrySize(1), entryCount(1)
        // Followed by entries: id(4), dmemOffset(4)
        uint8_t *intfHdr = dmem + fwsecInfo.interfaceOffset;
        if (fwsecInfo.interfaceOffset + 4 <= dmemSize) {
            uint8_t entryCount = intfHdr[3];
            uint32_t entryOffset = fwsecInfo.interfaceOffset + 4;

            for (int i = 0; i < entryCount && entryOffset + 8 <= dmemSize; i++) {
                uint32_t entryId = *(uint32_t *)(dmem + entryOffset);
                uint32_t dmemOff = *(uint32_t *)(dmem + entryOffset + 4);

                if (entryId == 0x4) {  // DMEMMAPPER entry ID
                    if (dmemOff + sizeof(DmemMapperHeader) <= dmemSize) {
                        mapper = (DmemMapperHeader *)(dmem + dmemOff);
                        IOLog("NVDAAL-GSP: Found DMEMMAPPER via interface at DMEM+0x%x\n", dmemOff);
                    }
                    break;
                }
                entryOffset += 8;
            }
        }
    }

    if (!mapper) {
        IOLog("NVDAAL-GSP: DMEMMAPPER not found in DMEM\n");
        return false;
    }

    // Verify DMEMMAPPER signature
    if (mapper->signature != DMEMMAPPER_SIGNATURE) {
        IOLog("NVDAAL-GSP: Invalid DMEMMAPPER signature: 0x%08x (expected 0x%08x)\n",
              mapper->signature, DMEMMAPPER_SIGNATURE);
        return false;
    }

    // Patch init_cmd to FRTS (0x15)
    IOLog("NVDAAL-GSP: Patching DMEMMAPPER: initCmd 0x%x -> 0x%x\n",
          mapper->initCmd, DMEMMAPPER_CMD_FRTS);
    mapper->initCmd = DMEMMAPPER_CMD_FRTS;

    // Patch command buffer with FRTS command structure
    if (mapper->cmdBufOffset + sizeof(FwsecFrtsCmd) <= dmemSize) {
        FwsecFrtsCmd *frtsCmd = (FwsecFrtsCmd *)(dmem + mapper->cmdBufOffset);

        // Fill readVbios descriptor
        frtsCmd->readVbios.version = 1;
        frtsCmd->readVbios.size = sizeof(FwsecReadVbiosDesc);
        frtsCmd->readVbios.gfwImageOffset = 0;
        frtsCmd->readVbios.gfwImageSize = 0;
        frtsCmd->readVbios.flags = 2;  // FWSECLIC_READ_VBIOS_STRUCT_FLAGS

        // Fill FRTS region descriptor
        frtsCmd->frtsRegion.version = 1;
        frtsCmd->frtsRegion.size = sizeof(FwsecFrtsRegionDesc);
        frtsCmd->frtsRegion.frtsOffset4K = (uint32_t)(frtsOffset >> 12);
        frtsCmd->frtsRegion.frtsSize4K = 0x100;  // 1MB in 4K units
        frtsCmd->frtsRegion.mediaType = 2;  // FB

        IOLog("NVDAAL-GSP: FRTS cmd patched: offset4K=0x%x, size4K=0x%x, type=%d\n",
              frtsCmd->frtsRegion.frtsOffset4K, frtsCmd->frtsRegion.frtsSize4K,
              frtsCmd->frtsRegion.mediaType);
    } else {
        IOLog("NVDAAL-GSP: Warning: cmdBufOffset 0x%x out of bounds, FRTS cmd not patched\n",
              mapper->cmdBufOffset);
    }

    return true;
}

bool NVDAALGsp::executeFwsecFrts(void) {
    IOLog("NVDAAL-GSP: Executing FWSEC-FRTS...\n");

    // First, check if WPR2 is already set up (by EFI/GOP/VBIOS POST)
    uint32_t wpr2HiReg = readReg(NV_PFB_PRI_MMU_WPR2_ADDR_HI);
    uint32_t wpr2LoReg = readReg(NV_PFB_PRI_MMU_WPR2_ADDR_LO);
    IOLog("NVDAAL-GSP: WPR2 registers: HI=0x%08x LO=0x%08x\n", wpr2HiReg, wpr2LoReg);

    if (checkWpr2Setup()) {
        IOLog("NVDAAL-GSP: WPR2 already configured by EFI/VBIOS!\n");
        return true;
    }

    // If no VBIOS loaded from file, try reading from PROM
    // PROM contains VBIOS after GPU POST with FWSEC unencrypted
    if (!fwsecMem && !vbiosMem) {
        IOLog("NVDAAL-GSP: No VBIOS loaded, trying PROM reading (post-POST)...\n");
        if (readVbiosFromProm()) {
            IOLog("NVDAAL-GSP: Successfully read VBIOS from PROM!\n");
            // Use vbiosMem instead of fwsecMem for parsing
            fwsecMem = vbiosMem;
            fwsecPhys = vbiosPhys;
        } else {
            IOLog("NVDAAL-GSP: Failed to read VBIOS from PROM\n");
        }
    }

    if (!fwsecMem) {
        IOLog("NVDAAL-GSP: No VBIOS available (file or PROM), cannot run FWSEC\n");
        IOLog("NVDAAL-GSP: WPR2 not configured - GSP may not boot correctly\n");
        return false;
    }

    const uint8_t *vbiosData = (const uint8_t *)fwsecMem->getBytesNoCopy();
    size_t vbiosSize = fwsecMem->getLength();

    // Parse VBIOS if not already done
    if (!fwsecInfo.valid) {
        if (!parseVbios(vbiosData, vbiosSize)) {
            IOLog("NVDAAL-GSP: Failed to parse VBIOS - no FWSEC found\n");
            IOLog("NVDAAL-GSP: Continuing without FWSEC (WPR2 may be pre-configured)\n");
            return false;
        }
    }

    if (!fwsecInfo.valid) {
        IOLog("NVDAAL-GSP: FWSEC ucode not found in VBIOS\n");
        return false;
    }

    // === Method 1: BROM HS DMA (preferred - matches NVIDIA GA102 path) ===
    // Uses DMA to load IMEM/DMEM with SEC bit, then BROM verifies + executes
    IOLog("NVDAAL-GSP: Method 1: BROM HS DMA execution...\n");
    if (executeFwsecViaBrom()) {
        IOLog("NVDAAL-GSP: BROM HS DMA method succeeded!\n");
        return true;
    }

    // === Method 2: PIO + BROM HS (fallback - PIO load, BROM verify) ===
    IOLog("NVDAAL-GSP: Method 2: PIO + BROM HS on GSP Falcon...\n");
    {
        // FWSEC runs on GSP Falcon in HS mode (booter runs on SEC2)
        const uint32_t falconBase = NV_PGSP_BASE;
        const void *imem = vbiosData + fwsecInfo.imemOffset;

        if (fwsecInfo.imemOffset + fwsecInfo.imemSize > vbiosSize ||
            fwsecInfo.dmemOffset + fwsecInfo.dmemSize > vbiosSize) {
            IOLog("NVDAAL-GSP: Invalid FWSEC offsets\n");
            return false;
        }

        // Prepare patched DMEM
        uint8_t *dmemCopy = (uint8_t *)IOMalloc(fwsecInfo.dmemSize);
        if (!dmemCopy) {
            IOLog("NVDAAL-GSP: Failed to allocate DMEM copy\n");
            return false;
        }
        memcpy(dmemCopy, vbiosData + fwsecInfo.dmemOffset, fwsecInfo.dmemSize);

        // Patch signature
        if (fwsecInfo.signatureCount > 0 && fwsecInfo.pkcDataOffset > 0) {
            patchFwsecSignature(dmemCopy, fwsecInfo.dmemSize);
        }

        // Patch FRTS command
        uint64_t frtsOffset = 0;
        if (!patchFrtsCmdBuffer(dmemCopy, fwsecInfo.dmemSize, frtsOffset)) {
            if (fwsecInfo.dmemMapperOffset > 0 &&
                fwsecInfo.dmemMapperOffset + sizeof(DmemMapperHeader) <= fwsecInfo.dmemSize) {
                DmemMapperHeader *mapper = (DmemMapperHeader *)(dmemCopy + fwsecInfo.dmemMapperOffset);
                mapper->initCmd = DMEMMAPPER_CMD_FRTS;
            }
        }

        // Engine reset (GA102 sequence)
        for (int i = 0; i < 150; i++) {
            if (readReg(falconBase + FALCON_HWCFG2) & FALCON_HWCFG2_RESET_READY) break;
            IODelay(1);
        }
        writeReg(falconBase + FALCON_ENGINE, 1);
        for (int i = 0; i < 10; i++) readReg(falconBase + FALCON_ENGINE);
        writeReg(falconBase + FALCON_ENGINE, 0);
        for (int i = 0; i < 10; i++) readReg(falconBase + FALCON_ENGINE);

        // Wait for scrub (HWCFG2 bit 12)
        for (int i = 0; i < 50000; i++) {
            if (!(readReg(falconBase + FALCON_HWCFG2) & FALCON_HWCFG2_MEM_SCRUBBING)) break;
            IODelay(1);
        }

        // Switch to Falcon core
        writeReg(falconBase + FALCON_BCR_CTRL, 0);
        for (int i = 0; i < 10000; i++) {
            IODelay(1);
            if (readReg(falconBase + FALCON_BCR_CTRL) & FALCON_BCR_CTRL_VALID) break;
        }

        // Set RM register
        writeReg(falconBase + FALCON_RM, readReg(NV_PMC_BOOT_0));

        // DisableCtxReq
        uint32_t fbifCtl = readReg(falconBase + FALCON_FBIF_CTL);
        fbifCtl |= FALCON_FBIF_CTL_ALLOW_PHYS_NO_CTX;
        writeReg(falconBase + FALCON_FBIF_CTL, fbifCtl);
        writeReg(falconBase + FALCON_DMACTL, 0);

        // Configure TRANSCFG
        uint32_t transcfg = readReg(falconBase + FALCON_FBIF_TRANSCFG(0));
        transcfg &= ~0x07;
        transcfg |= FALCON_TRANSCFG_TARGET_COHERENT | (1 << 2);
        writeReg(falconBase + FALCON_FBIF_TRANSCFG(0), transcfg);

        // PIO load with SECURE+tags
        loadFalconUcode(falconBase, imem, fwsecInfo.imemSize, dmemCopy, fwsecInfo.dmemSize);
        IOFree(dmemCopy, fwsecInfo.dmemSize);

        // Set BROM parameters
        writeReg(falconBase + FALCON_BROM_PARAADDR, fwsecInfo.pkcDataOffset);
        writeReg(falconBase + FALCON_BROM_ENGIDMASK, fwsecInfo.signatureVersions);
        writeReg(falconBase + FALCON_BROM_CURR_UCODE_ID, fwsecInfo.ucodeId);
        writeReg(falconBase + FALCON_BROM_MOD_SEL, 1);

        // Set boot vector
        writeReg(falconBase + FALCON_BOOTVEC, fwsecInfo.bootVec);

        // Start via CPUCTL_ALIAS (HS mode)
        IOLog("NVDAAL-GSP: Starting Falcon via CPUCTL_ALIAS...\n");
        writeReg(falconBase + FALCON_CPUCTL_ALIAS, FALCON_CPUCTL_STARTCPU);

        // Wait for halt
        bool halted = false;
        for (int i = 0; i < 5000; i++) {
            uint32_t cpuctl = readReg(falconBase + FALCON_CPUCTL);
            if (cpuctl & FALCON_CPUCTL_HALTED) {
                uint32_t mbox0 = readReg(falconBase + FALCON_MAILBOX0);
                IOLog("NVDAAL-GSP: Falcon halted: CPUCTL=0x%08x MBOX0=0x%08x\n", cpuctl, mbox0);
                halted = true;
                break;
            }
            IODelay(1000);
        }

        if (halted && checkWpr2Setup()) {
            IOLog("NVDAAL-GSP: PIO+BROM HS method succeeded!\n");
            return true;
        }
    }

    IOLog("NVDAAL-GSP: FWSEC-FRTS: all methods failed, WPR2 not configured\n");
    return false;
}

bool NVDAALGsp::executeBooterLoad(void) {
    // Execute booter_load on SEC2 (Ada Lovelace)
    // Booter authenticates and loads GSP-RM into WPR2

    IOLog("NVDAAL-GSP: Executing booter_load on SEC2...\n");

    if (!booterLoadMem) {
        IOLog("NVDAAL-GSP: No booter_load firmware\n");
        return false;
    }

    size_t booterSize = booterLoadMem->getLength();
    if (booterSize < 256) {
        IOLog("NVDAAL-GSP: booter_load too small\n");
        return false;
    }

    IOLog("NVDAAL-GSP: booter_load size: %lu bytes @ phys 0x%llx\n", 
          (unsigned long)booterSize, booterLoadPhys);

    // Diagnostic: Read SEC2 hardware config
    uint32_t hwcfg = readReg(NV_PSEC_FALCON_HWCFG);
    uint32_t falconCpuctl = readReg(NV_PSEC_FALCON_CPUCTL);
    uint32_t riscvCpuctl = readReg(NV_PSEC_RISCV_CPUCTL);
    IOLog("NVDAAL-GSP: SEC2 HWCFG=0x%08x FALCON_CPUCTL=0x%08x RISCV_CPUCTL=0x%08x\n",
          hwcfg, falconCpuctl, riscvCpuctl);
    
    // HWCFG bits: [3:0]=IMEM_SIZE, [7:4]=DMEM_SIZE, [8]=RISCV
    bool isRiscv = (hwcfg >> 8) & 1;
    IOLog("NVDAAL-GSP: SEC2 is %s core\n", isRiscv ? "RISC-V" : "Falcon");

    // Read current SEC2 RISC-V state
    uint32_t cpuctl = readReg(NV_PSEC_RISCV_CPUCTL);
    IOLog("NVDAAL-GSP: SEC2 RISCV_CPUCTL initial: 0x%08x\n", cpuctl);

    // For RISC-V SEC2, we use the BCR (Boot Config Region) mechanism
    // The booter firmware address goes into BCR_DMEM_ADDR
    // BCR_CTRL triggers the boot
    
    // Setup WPR meta address in Falcon mailboxes (booter reads these)
    uint32_t mailbox0 = (uint32_t)(wprMetaPhys & 0xFFFFFFFF);
    uint32_t mailbox1 = (uint32_t)(wprMetaPhys >> 32);
    writeReg(NV_PSEC_FALCON_MAILBOX0, mailbox0);
    writeReg(NV_PSEC_FALCON_MAILBOX1, mailbox1);
    IOLog("NVDAAL-GSP: SEC2 mailbox set to WPR meta @ 0x%llx\n", wprMetaPhys);

    // Also set GSP firmware info in additional mailboxes if available
    // Mailbox format: [0]=wprMeta_lo, [1]=wprMeta_hi, [2]=gspFw_lo, [3]=gspFw_hi
    
    // Configure BCR for RISC-V boot
    // BCR_DMEM_ADDR = physical address of booter >> 8 (256-byte aligned)
    uint32_t bcrDmemAddr = (uint32_t)(booterLoadPhys >> 8);
    writeReg(NV_PSEC_RISCV_BCR_DMEM_ADDR, bcrDmemAddr);
    IOLog("NVDAAL-GSP: SEC2 BCR_DMEM_ADDR = 0x%08x\n", bcrDmemAddr);

    // BCR_CTRL: bit 0 = trigger boot, bits 31:8 = boot config
    // For HS (Heavy Secure) boot: typically just set bit 0
    uint32_t bcrCtrl = readReg(NV_PSEC_RISCV_BCR_CTRL);
    IOLog("NVDAAL-GSP: SEC2 BCR_CTRL before: 0x%08x\n", bcrCtrl);
    
    // Set BCR_CTRL to trigger boot (bit 0 = 1)
    writeReg(NV_PSEC_RISCV_BCR_CTRL, bcrDmemAddr | 0x1);
    IOLog("NVDAAL-GSP: SEC2 BCR_CTRL set to: 0x%08x\n", bcrDmemAddr | 0x1);

    // Start the RISC-V core
    // CPUCTL: bit 1 = start CPU
    cpuctl = readReg(NV_PSEC_RISCV_CPUCTL);
    writeReg(NV_PSEC_RISCV_CPUCTL, cpuctl | 0x2);  // Set START bit
    IOLog("NVDAAL-GSP: SEC2 RISCV start command sent\n");

    // Wait for SEC2 to finish (halted or mailbox response)
    for (int i = 0; i < 200; i++) {  // 200ms timeout
        cpuctl = readReg(NV_PSEC_RISCV_CPUCTL);
        
        // Check if halted (bit 4) or check mailbox
        if (cpuctl & 0x10) {
            // Check return code
            uint32_t retcode = readReg(NV_PSEC_RISCV_BR_RETCODE);
            uint32_t result = readReg(NV_PSEC_FALCON_MAILBOX0);
            IOLog("NVDAAL-GSP: SEC2 halted, CPUCTL=0x%08x RETCODE=0x%08x MB0=0x%08x\n", 
                  cpuctl, retcode, result);

            if (retcode == 0 || result == 0) {
                IOLog("NVDAAL-GSP: booter_load completed successfully\n");
                return true;
            } else {
                IOLog("NVDAAL-GSP: booter_load failed: retcode=0x%x mb0=0x%x\n", retcode, result);
                return false;
            }
        }
        
        // Check if still starting
        if (i == 10) {
            IOLog("NVDAAL-GSP: SEC2 still running, CPUCTL=0x%08x\n", cpuctl);
        }
        
        IODelay(1000);  // 1ms
    }

    // Final state dump
    cpuctl = readReg(NV_PSEC_RISCV_CPUCTL);
    uint32_t retcode = readReg(NV_PSEC_RISCV_BR_RETCODE);
    uint32_t bcrCtrlFinal = readReg(NV_PSEC_RISCV_BCR_CTRL);
    IOLog("NVDAAL-GSP: SEC2 timeout: CPUCTL=0x%08x RETCODE=0x%08x BCR=0x%08x\n", 
          cpuctl, retcode, bcrCtrlFinal);

    IOLog("NVDAAL-GSP: Timeout waiting for SEC2/booter\n");
    return false;
}

bool NVDAALGsp::startRiscv(void) {
    IOLog("NVDAAL-GSP: Starting RISC-V core...\n");

    // Debug: Scan for valid RISC-V registers
    IOLog("NVDAAL-GSP: Scanning for RISC-V registers...\n");
    
    // Test different base addresses for GSP RISC-V
    uint32_t testBases[] = {0x110000, 0x111000, 0x112000, 0x113000, 0x117000, 0x118000, 0x119000};
    for (int i = 0; i < 7; i++) {
        uint32_t base = testBases[i];
        uint32_t val388 = readReg(base + 0x388);  // CPUCTL offset
        uint32_t val100 = readReg(base + 0x100);  // Falcon CPUCTL offset
        if (val388 != 0xbadf5620 && val388 != 0xffffffff) {
            IOLog("NVDAAL-GSP: Found RISC-V at base 0x%06x: CPUCTL=0x%08x\n", base, val388);
        }
        if (val100 != 0xbadf5620 && val100 != 0xffffffff) {
            IOLog("NVDAAL-GSP: Found Falcon at base 0x%06x: CPUCTL=0x%08x\n", base, val100);
        }
    }

    // Read current state
    uint32_t preCpuctl = readReg(NV_PRISCV_RISCV_CPUCTL);
    uint32_t preBcrCtrl = readReg(NV_PRISCV_RISCV_BCR_CTRL);
    IOLog("NVDAAL-GSP: Pre-start: CPUCTL=0x%08x BCR_CTRL=0x%08x\n", preCpuctl, preBcrCtrl);
    IOLog("NVDAAL-GSP: WPR Meta @ 0x%llx, Radix3 @ 0x%llx\n", wprMetaPhys, radix3Phys);

    // Configure boot config register with WPR meta address
    uint32_t bcrAddr = (uint32_t)(wprMetaPhys >> 8);  // 256-byte aligned
    IOLog("NVDAAL-GSP: Setting BCR_DMEM_ADDR=0x%08x\n", bcrAddr);
    writeReg(NV_PRISCV_RISCV_BCR_DMEM_ADDR, bcrAddr);
    
    uint32_t bcrCtrlVal = NV_PRISCV_RISCV_BCR_CTRL_VALID | bcrAddr;
    IOLog("NVDAAL-GSP: Setting BCR_CTRL=0x%08x\n", bcrCtrlVal);
    writeReg(NV_PRISCV_RISCV_BCR_CTRL, bcrCtrlVal);

    // Start the core
    IOLog("NVDAAL-GSP: Writing CPUCTL START command\n");
    writeReg(NV_PRISCV_RISCV_CPUCTL, NV_PRISCV_CPUCTL_START);

    // Wait for core to become active
    for (int i = 0; i < 100; i++) {
        uint32_t status = readReg(NV_PRISCV_RISCV_CPUCTL);
        uint32_t retcode = readReg(NV_PRISCV_RISCV_BR_RETCODE);
        
        if (i == 0 || i == 10 || i == 50 || i == 99) {
            IOLog("NVDAAL-GSP: [%d] CPUCTL=0x%08x BR_RETCODE=0x%08x\n", i, status, retcode);
        }
        
        if (status & NV_PRISCV_CPUCTL_ACTIVE) {
            IOLog("NVDAAL-GSP: RISC-V core active after %d iterations\n", i);
            return true;
        }
        
        // Check for error codes
        if (retcode != 0 && retcode != 0xbadf5040) {
            IOLog("NVDAAL-GSP: Boot error detected: BR_RETCODE=0x%08x at iteration %d\n", retcode, i);
        }
        
        IODelay(1000);  // 1ms delay
    }

    // Final state dump
    uint32_t finalCpuctl = readReg(NV_PRISCV_RISCV_CPUCTL);
    uint32_t finalRetcode = readReg(NV_PRISCV_RISCV_BR_RETCODE);
    uint32_t scratch14 = readReg(NV_PGC6_BSI_SECURE_SCRATCH_14);
    uint32_t mailbox0 = readReg(NV_PGSP_FALCON_MAILBOX0);
    IOLog("NVDAAL-GSP: Final: CPUCTL=0x%08x RETCODE=0x%08x SCRATCH14=0x%08x MB0=0x%08x\n",
          finalCpuctl, finalRetcode, scratch14, mailbox0);

    IOLog("NVDAAL-GSP: RISC-V core failed to start\n");
    return false;
}

bool NVDAALGsp::waitForInitDone(uint32_t timeoutMs) {
    IOLog("NVDAAL-GSP: Waiting for GSP_INIT_DONE...\n");

    uint32_t elapsed = 0;
    while (elapsed < timeoutMs) {
        // Check mailbox for init done signal
        uint32_t mailbox = readReg(NV_PGSP_FALCON_MAILBOX0);
        if (mailbox == NV_VGPU_MSG_EVENT_GSP_INIT_DONE) {
            IOLog("NVDAAL-GSP: GSP_INIT_DONE received!\n");
            gspReady = true;
            return true;
        }

        // Also check status queue for init done event
        // TODO: Implement proper queue polling

        IODelay(10000);  // 10ms delay
        elapsed += 10;
    }

    IOLog("NVDAAL-GSP: Timeout waiting for GSP_INIT_DONE\n");
    return false;
}

uint32_t NVDAALGsp::getBootStatus(void) const {
    if (!mmioBase) return 0xFFFFFFFF;
    return mmioBase[NV_PRISCV_RISCV_BR_RETCODE / 4];
}

// ============================================================================
// RPC Communication
// ============================================================================

uint32_t NVDAALGsp::calcChecksum(const void *data, size_t size) {
    // Simple CRC32-like checksum
    uint32_t crc = 0xFFFFFFFF;
    const uint8_t *bytes = (const uint8_t *)data;

    for (size_t i = 0; i < size; i++) {
        crc ^= bytes[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
        }
    }

    return ~crc;
}

bool NVDAALGsp::enqueueCommand(const void *msg, size_t size) {
    if (!cmdQueue) return false;

    // Calculate total element size (header + payload)
    size_t elemSize = sizeof(GspQueueElement) + size;
    size_t alignedSize = (elemSize + 0xFFULL) & ~0xFFULL;  // 256-byte aligned

    // Check for queue space
    uint32_t head = cmdQueueHead;
    uint32_t tail = cmdQueueTail;
    size_t freeSpace = (tail >= head) ? (QUEUE_SIZE - tail + head) : (head - tail);

    if (freeSpace < alignedSize) {
        IOLog("NVDAAL-GSP: Command queue full\n");
        return false;
    }

    // Write queue element
    GspQueueElement *elem = (GspQueueElement *)(cmdQueue + tail);
    elem->seqNum = rpcSeqNum++;
    elem->elemCount = (uint32_t)((alignedSize + 0xFFF) / 0x1000);  // 4KB pages
    elem->reserved = 0;

    // Copy message payload
    memcpy(elem->data, msg, size);

    // Calculate checksum
    elem->checkSum = calcChecksum(elem->data, size);

    // Update tail
    cmdQueueTail = (tail + alignedSize) % QUEUE_SIZE;

    // Update hardware queue pointer
    writeReg(NV_PGSP_QUEUE_TAIL(GSP_CMDQ_IDX), cmdQueueTail);

    return true;
}

bool NVDAALGsp::sendRpc(uint32_t function, const void *params, size_t paramsSize) {
    // SET_SYSTEM_INFO and SET_REGISTRY are async RPCs queued BEFORE GSP boots
    if (!gspReady &&
        function != NV_VGPU_MSG_FUNCTION_GSP_SET_SYSTEM_INFO &&
        function != NV_VGPU_MSG_FUNCTION_SET_REGISTRY) {
        IOLog("NVDAAL-GSP: GSP not ready\n");
        return false;
    }

    // Build RPC message
    size_t msgSize = sizeof(NvRpcMessageHeader) + paramsSize;
    uint8_t *msgBuf = (uint8_t *)IOMalloc(msgSize);
    if (!msgBuf) return false;

    NvRpcMessageHeader *header = (NvRpcMessageHeader *)msgBuf;
    header->signature = NV_VGPU_MSG_SIGNATURE_VALID;
    header->headerVersion = (3 << 24);
    header->rpcResult = 0;
    header->rpcResultPriv = 0;
    header->function = function;
    header->length = (uint32_t)msgSize;

    if (params && paramsSize > 0) {
        memcpy(msgBuf + sizeof(NvRpcMessageHeader), params, paramsSize);
    }

    // Enqueue
    bool ok = enqueueCommand(msgBuf, msgSize);

    IOFree(msgBuf, msgSize);

    if (ok) {
        IOLog("NVDAAL-GSP: RPC 0x%02x sent\n", function);
    }

    return ok;
}

void NVDAALGsp::updateQueuePointers(void) {
    // Sync local pointers with hardware
    cmdQueueHead = readReg(NV_PGSP_QUEUE_HEAD(GSP_CMDQ_IDX));
    statQueueHead = readReg(NV_PGSP_QUEUE_HEAD(GSP_MSGQ_IDX));
}

bool NVDAALGsp::dequeueStatus(void *msg, size_t maxSize, size_t *actualSize) {
    if (!statQueue) return false;

    // Update hardware head
    statQueueHead = readReg(NV_PGSP_QUEUE_HEAD(GSP_MSGQ_IDX));
    
    if (statQueueHead == statQueueTail) return false;

    GspQueueElement *elem = (GspQueueElement *)(statQueue + statQueueTail);
    
    // Verify checksum
    // size_t payloadSize = ... (Need to know actual msg size)
    // For now, use the elemCount to determine total size
    size_t totalSize = elem->elemCount * 0x1000;
    size_t payloadSize = totalSize - sizeof(GspQueueElement);

    if (payloadSize > maxSize) {
        payloadSize = maxSize;
    }

    memcpy(msg, elem->data, payloadSize);
    if (actualSize) *actualSize = payloadSize;

    // Increment tail by aligned size
    statQueueTail = (statQueueTail + totalSize) % QUEUE_SIZE;
    
    // Tell GSP we consumed the message
    writeReg(NV_PGSP_QUEUE_TAIL(GSP_MSGQ_IDX), statQueueTail);

    return true;
}

bool NVDAALGsp::waitRpcResponse(uint32_t function, void *response, size_t responseSize, uint32_t timeoutMs) {
    uint32_t elapsed = 0;
    uint8_t rpcBuf[4096];
    size_t actualSize = 0;

    while (elapsed < timeoutMs) {
        if (dequeueStatus(rpcBuf, sizeof(rpcBuf), &actualSize)) {
            NvRpcMessageHeader *hdr = (NvRpcMessageHeader *)rpcBuf;
            
            if (hdr->signature == NV_VGPU_MSG_SIGNATURE_VALID && hdr->function == function) {
                if (response && responseSize > 0) {
                    size_t copySize = (hdr->length - sizeof(NvRpcMessageHeader));
                    if (copySize > responseSize) copySize = responseSize;
                    memcpy(response, rpcBuf + sizeof(NvRpcMessageHeader), copySize);
                }
                return true;
            }
            
            // If it's another event (like INIT_DONE), handle it?
            if (hdr->function == NV_VGPU_MSG_EVENT_GSP_INIT_DONE) {
                IOLog("NVDAAL-GSP: Async GSP_INIT_DONE received\n");
                gspReady = true;
            }
        }

        IODelay(100); // 100us
        elapsed++;
    }

    return false;
}

// ============================================================================
// Resource Manager (RM) Implementation
// ============================================================================

bool NVDAALGsp::rmAlloc(uint32_t hClient, uint32_t hParent, uint32_t hObject, uint32_t hClass, void *params, size_t paramsSize) {
    size_t allocSize = sizeof(NvGspAllocParams) + paramsSize;
    uint8_t stackBuf[256];
    uint8_t *buffer = stackBuf;
    bool allocated = false;

    // Optimization: Use stack buffer for small requests to avoid heap overhead
    if (allocSize > sizeof(stackBuf)) {
        buffer = (uint8_t *)IOMalloc(allocSize);
        if (!buffer) return false;
        allocated = true;
    }

    NvGspAllocParams *header = (NvGspAllocParams *)buffer;
    header->hClient = hClient;
    header->hParent = hParent;
    header->hObject = hObject;
    header->hClass = hClass;
    header->status = 0;

    if (params && paramsSize > 0) {
        memcpy(buffer + sizeof(NvGspAllocParams), params, paramsSize);
    }

    bool result = sendRpc(NV_VGPU_MSG_FUNCTION_GSP_RM_ALLOC, buffer, allocSize);
    
    // Check status if RPC succeeded
    if (result && header->status != 0) {
        IOLog("NVDAAL-GSP: rmAlloc failed with RM status 0x%x\n", header->status);
        result = false;
    }

    if (allocated) {
        IOFree(buffer, allocSize);
    }
    return result;
}

bool NVDAALGsp::rmControl(uint32_t hClient, uint32_t hObject, uint32_t cmd, void *params, size_t paramsSize) {
    size_t ctrlSize = sizeof(NvGspControlParams) + paramsSize;
    uint8_t stackBuf[256];
    uint8_t *buffer = stackBuf;
    bool allocated = false;

    if (ctrlSize > sizeof(stackBuf)) {
        buffer = (uint8_t *)IOMalloc(ctrlSize);
        if (!buffer) return false;
        allocated = true;
    }

    NvGspControlParams *header = (NvGspControlParams *)buffer;
    header->hClient = hClient;
    header->hObject = hObject;
    header->cmd = cmd;
    header->flags = 0;
    header->status = 0;
    header->paramsSize = (uint32_t)paramsSize;

    if (params && paramsSize > 0) {
        memcpy(buffer + sizeof(NvGspControlParams), params, paramsSize);
    }

    bool result = sendRpc(NV_VGPU_MSG_FUNCTION_GSP_RM_CONTROL, buffer, ctrlSize);

    if (result && header->status != 0) {
        IOLog("NVDAAL-GSP: rmControl failed with RM status 0x%x\n", header->status);
        result = false;
    }

    if (allocated) {
        IOFree(buffer, ctrlSize);
    }
    return result;
}

bool NVDAALGsp::rmFree(uint32_t hClient, uint32_t hParent, uint32_t hObject) {
    uint32_t params[3];
    params[0] = hClient;
    params[1] = hParent;
    params[2] = hObject;

    return sendRpc(NV_VGPU_MSG_FUNCTION_GSP_RM_FREE, params, sizeof(params));
}


bool NVDAALGsp::sendSystemInfo(void) {
    // ========================================================================
    // THE MACUMBA - Make GSP firmware think we're running on Linux
    // Source: rpcGspSetSystemInfo_v17_00() in NVIDIA open-gpu-kernel-modules
    // ========================================================================

    GspSystemInfo info;
    memset(&info, 0, sizeof(info));

    IOLog("NVDAAL-GSP: GspSystemInfo struct size: %lu bytes\n", (unsigned long)sizeof(info));

    if (!pciDevice) {
        IOLog("NVDAAL-GSP: ERROR - no PCI device for system info\n");
        return false;
    }

    // ====================================================================
    // Physical addresses (from PCI BARs)
    // RTX 4090 uses 64-bit BARs: BAR0 @ 0x10-0x17, BAR1 @ 0x18-0x1F
    // ====================================================================
    uint32_t bar0Lo = pciDevice->configRead32(0x10);
    uint32_t bar0Hi = pciDevice->configRead32(0x14);
    uint32_t bar1Lo = pciDevice->configRead32(0x18);
    uint32_t bar1Hi = pciDevice->configRead32(0x1C);

    // Check if 64-bit BAR (bit 2:1 of BAR = 10b means 64-bit)
    bool bar0is64 = ((bar0Lo & 0x6) == 0x4);
    bool bar1is64 = ((bar1Lo & 0x6) == 0x4);

    info.gpuPhysAddr = (uint64_t)(bar0Lo & 0xFFFFFFF0UL);
    if (bar0is64) info.gpuPhysAddr |= ((uint64_t)bar0Hi << 32);

    info.gpuPhysFbAddr = (uint64_t)(bar1Lo & 0xFFFFFFF0UL);
    if (bar1is64) info.gpuPhysFbAddr |= ((uint64_t)bar1Hi << 32);

    // Instance memory = same as BAR0 (MMIO) for Ada
    info.gpuPhysInstAddr = info.gpuPhysAddr;
    info.gpuPhysIoAddr = 0;  // Modern GPUs don't use I/O ports

    IOLog("NVDAAL-GSP: BAR0 phys = 0x%llx, BAR1 phys = 0x%llx\n",
          info.gpuPhysAddr, info.gpuPhysFbAddr);

    // ====================================================================
    // PCI Bus/Device/Function encoding
    // Format: (domain << 32) | (bus << 8) | (device << 3) | function
    // ====================================================================
    // Read from IOPCIDevice properties
    OSNumber *busNum = OSDynamicCast(OSNumber, pciDevice->getProperty("bus-number"));
    OSNumber *devNum = OSDynamicCast(OSNumber, pciDevice->getProperty("device-number"));
    OSNumber *funNum = OSDynamicCast(OSNumber, pciDevice->getProperty("function-number"));

    uint32_t pciBus = busNum ? busNum->unsigned32BitValue() : 0;
    uint32_t pciDev = devNum ? devNum->unsigned32BitValue() : 0;
    uint32_t pciFun = funNum ? funNum->unsigned32BitValue() : 0;

    info.nvDomainBusDeviceFunc = ((uint64_t)0 << 32) |
                                 ((uint64_t)pciBus << 8) |
                                 ((uint64_t)(pciDev & 0x1F) << 3) |
                                 ((uint64_t)(pciFun & 0x7));

    IOLog("NVDAAL-GSP: PCI BDF = %u:%u.%u (encoded 0x%llx)\n",
          pciBus, pciDev, pciFun, info.nvDomainBusDeviceFunc);

    // ====================================================================
    // PCI identification
    // ====================================================================
    info.PCIDeviceID    = pciDevice->configRead16(0x02);      // 0x2684 for RTX 4090
    info.PCISubDeviceID = pciDevice->configRead16(0x2E);      // 0x889D for ASUS ROG
    info.PCIRevisionID  = pciDevice->configRead8(0x08);       // 0xA1

    // ====================================================================
    // Memory and VM layout
    // ====================================================================
    info.simAccessBufPhysAddr = 0;             // Not a simulator
    info.notifyOpSharedSurfacePhysAddr = 0;    // Set later during RM init
    info.pcieAtomicsOpMask = 0;                // PCIe atomics not configured
    info.consoleMemSize = 0;                   // No console display (compute-only)

    // KEY MACUMBA VALUE: Linux x86_64 maxUserVa = 0x00007FFFFFFFFFFF (47-bit)
    info.maxUserVa = 0x00007FFFFFFFFFFFULL;

    info.pciConfigMirrorBase = 0x88000;        // Standard PCI config mirror base
    info.pciConfigMirrorSize = 0x1000;         // 4KB mirror

    info.pcieAtomicsCplDeviceCapMask = 0;
    info.oorArch = 0;

    // ====================================================================
    // Chipset info (Intel Z790 Raptor Lake)
    // ====================================================================
    info.clPdbProperties = 0;                  // No special chipset properties
    info.Chipset = 0xA780;                     // Intel Z790 host bridge

    info.bGpuBehindBridge = 1;                 // GPU is behind PCIe bridge
    info.bFlrSupported = 1;                    // Function Level Reset
    info.b64bBar0Supported = bar0is64 ? 1 : 0;
    info.bMnocAvailable = 0;
    info.chipsetL1ssEnable = 0;                // No L1 substates
    info.bUpstreamL0sUnsupported = 0;
    info.bUpstreamL1Unsupported = 0;
    info.bUpstreamL1PorSupported = 0;
    info.bUpstreamL1PorMobileOnly = 0;
    info.bSystemHasMux = 0;                    // Desktop, no Optimus mux
    info.upstreamAddressValid = 1;

    // First Host Bridge bus info (Intel Z790)
    info.FHBBusInfo.deviceID    = 0xA780;      // Z790 host bridge
    info.FHBBusInfo.vendorID    = 0x8086;      // Intel
    info.FHBBusInfo.subdeviceID = 0;
    info.FHBBusInfo.subvendorID = 0x1043;      // ASUS
    info.FHBBusInfo.revisionID  = 0x01;

    // Chipset ID info (same as host bridge for desktop)
    info.chipsetIDInfo.deviceID    = 0xA780;
    info.chipsetIDInfo.vendorID    = 0x8086;
    info.chipsetIDInfo.subdeviceID = 0;
    info.chipsetIDInfo.subvendorID = 0x1043;
    info.chipsetIDInfo.revisionID  = 0x01;

    // ====================================================================
    // ACPI data - zeroed (Hackintosh has no NVIDIA ACPI methods)
    // ====================================================================
    info.acpiMethodData.bValid = 0;            // No valid ACPI data

    // ====================================================================
    // THE CORE MACUMBA - Virtualization & OS flags
    // These fields determine how GSP firmware behaves per platform
    // ====================================================================

    // Bare metal, not virtualized
    info.hypervisorType = OS_HYPERVISOR_UNKNOWN;  // 4 = bare metal
    info.virtualConfigBits = 0;
    info.bIsPassthru = 0;                         // Not GPU passthrough

    info.sysTimerOffsetNs = 0;                    // No timer offset

    // SR-IOV not used on desktop
    memset(&info.gspVFInfo, 0, sizeof(info.gspVFInfo));

    // === PRIMARY OS FLAGS - THE REAL MACUMBA ===
    info.bIsPrimary = 1;                               // Primary GPU
    info.bIsUnixHdmiFrlComplianceEnabled = 1;           // Unix flag (Linux sets this)
    info.isGridBuild = 0;                               // Not a GRID/vGPU build

    // PCIe link capability (Gen4 x16)
    info.pcieConfigReg.linkCap = 0x44;                  // Gen4 speed + x16 width
    info.gridBuildCsp = 0;

    // Feature flags
    info.bPreserveVideoMemoryAllocations = 0;           // No suspend/resume
    info.bTdrEventSupported = 0;                        // No TDR events (compute)
    info.bFeatureStretchVblankCapable = 0;              // No display
    info.bEnableDynamicGranularityPageArrays = 1;       // Linux enables this
    info.bClockBoostSupported = 0;

    // Host page size (same on Linux and macOS)
    info.hostPageSize = 4096;

    info.bIsCmcBasedHws = 0;

    // *** CRITICAL MACUMBA FLAG ***
    // On Windows: bGspNocatEnabled = true
    // On Linux:   bGspNocatEnabled = false
    // This is the #1 flag that distinguishes Windows from Linux in GSP!
    info.bGspNocatEnabled = 0;                          // NOT Windows!

    info.bS0ixSupport = 0;                              // No modern standby
    info.bWindowChannelAlwaysMapped = 0;                // Linux doesn't set this

    info.pciePowerControlValue = 0;
    info.bPciePowerControlPresent = 0;

    // ====================================================================
    // Log the macumba summary
    // ====================================================================

    // Check if SSDT specifies Linux-compat mode
    bool linuxCompatMode = false;
    OSString *bootMode = OSDynamicCast(OSString, pciDevice->getProperty("nvdaal-boot-mode"));
    if (bootMode && bootMode->isEqualTo("linux-compat")) {
        linuxCompatMode = true;
    }

    IOLog("NVDAAL-GSP: === GspSystemInfo (Linux-compat: %s) ===\n",
          linuxCompatMode ? "YES (from SSDT)" : "YES (default)");
    IOLog("NVDAAL-GSP:   DeviceID=0x%04x SubDevice=0x%04x Rev=0x%02x\n",
          info.PCIDeviceID, info.PCISubDeviceID, info.PCIRevisionID);
    IOLog("NVDAAL-GSP:   maxUserVa=0x%llx hostPageSize=%llu\n",
          info.maxUserVa, info.hostPageSize);
    IOLog("NVDAAL-GSP:   hypervisor=%u bIsPrimary=%u bIsPassthru=%u\n",
          info.hypervisorType, info.bIsPrimary, info.bIsPassthru);
    IOLog("NVDAAL-GSP:   bGspNocatEnabled=%u (MUST be 0 for Linux)\n",
          info.bGspNocatEnabled);
    IOLog("NVDAAL-GSP:   bIsUnixHdmiFrl=%u isGridBuild=%u\n",
          info.bIsUnixHdmiFrlComplianceEnabled, info.isGridBuild);
    IOLog("NVDAAL-GSP:   Chipset=0x%04x FHB=0x%04x:0x%04x\n",
          info.Chipset, info.FHBBusInfo.vendorID, info.FHBBusInfo.deviceID);
    IOLog("NVDAAL-GSP: ==============================================\n");

    return sendRpc(NV_VGPU_MSG_FUNCTION_GSP_SET_SYSTEM_INFO, &info, sizeof(info));
}

bool NVDAALGsp::setRegistry(const char *key, uint32_t value) {
    // Registry entries configure GSP behavior
    struct {
        char key[64];
        uint32_t value;
    } regEntry;

    memset(&regEntry, 0, sizeof(regEntry));
    strncpy(regEntry.key, key, 63);
    regEntry.value = value;

    IOLog("NVDAAL-GSP: Setting registry %s = %u\n", key, value);

    return sendRpc(NV_VGPU_MSG_FUNCTION_SET_REGISTRY, &regEntry, sizeof(regEntry));
}
