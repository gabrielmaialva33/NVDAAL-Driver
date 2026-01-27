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

    cmdQueueMem = nullptr;
    statQueueMem = nullptr;
    firmwareMem = nullptr;
    bootloaderMem = nullptr;
    wprMetaMem = nullptr;
    radix3Mem = nullptr;

    cmdQueue = nullptr;
    statQueue = nullptr;
    cmdQueueHead = 0;
    cmdQueueTail = 0;
    statQueueHead = 0;
    statQueueTail = 0;
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
    freeDmaBuffer(&bootloaderMem);
    freeDmaBuffer(&firmwareMem);
    freeDmaBuffer(&statQueueMem);
    freeDmaBuffer(&cmdQueueMem);

    cmdQueue = nullptr;
    statQueue = nullptr;
    mmioBase = nullptr;
    pciDevice = nullptr;
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

bool NVDAALGsp::loadFirmware(const char *firmwarePath) {
    // TODO: Load firmware from file
    // For now, firmware should be loaded externally and passed to loadBootloader
    IOLog("NVDAAL-GSP: loadFirmware not implemented - use loadBootloader\n");
    return false;
}

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
            
            // Allocate firmware memory
            if (!allocDmaBuffer(&firmwareMem, firmwareSize, &firmwarePhys)) {
                IOLog("NVDAAL-GSP: Failed to allocate firmware memory\n");
                return false;
            }

            // Copy firmware data
            memcpy(firmwareMem->getBytesNoCopy(), bytes + shdr->offset, shdr->size);
            
            // Build the page table for this firmware
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
    uint64_t fwPhys = firmwarePhys; // Base physical address of firmware blob
    
    for (uint64_t i = 0; i < numPages; i++) {
        // Each entry points to a 4KB page of the firmware
        l2Table[i] = (fwPhys + (i * GSP_PAGE_SIZE)) | 1; // Mark Valid
    }
    
    IOLog("NVDAAL-GSP: Radix3 built. Root: 0x%llx, Size: %lu bytes\n", 
          radix3Phys, tableSize);
    
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

bool NVDAALGsp::boot(void) {
    if (!initialized) {
        IOLog("NVDAAL-GSP: Not initialized\n");
        return false;
    }

    IOLog("NVDAAL-GSP: Starting boot sequence...\n");

    // Step 1: Reset FALCON
    if (!resetFalcon()) {
        IOLog("NVDAAL-GSP: FALCON reset failed\n");
        return false;
    }

    // Step 2: Execute FWSEC (from VBIOS)
    // TODO: This requires VBIOS parsing
    // if (!executeFwsec()) {
    //     return false;
    // }

    // Step 3: Setup WPR metadata
    if (!setupWprMeta()) {
        IOLog("NVDAAL-GSP: WPR meta setup failed\n");
        return false;
    }

    // Step 4: Start RISC-V core
    if (!startRiscv()) {
        IOLog("NVDAAL-GSP: RISC-V start failed\n");
        return false;
    }

    IOLog("NVDAAL-GSP: Boot sequence initiated\n");

    return true;
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

bool NVDAALGsp::startRiscv(void) {
    IOLog("NVDAAL-GSP: Starting RISC-V core...\n");

    // Configure boot config register with WPR meta address
    uint32_t bcrAddr = (uint32_t)(wprMetaPhys >> 8);  // 256-byte aligned
    writeReg(NV_PRISCV_RISCV_BCR_DMEM_ADDR, bcrAddr);
    writeReg(NV_PRISCV_RISCV_BCR_CTRL, NV_PRISCV_RISCV_BCR_CTRL_VALID | bcrAddr);

    // Start the core
    writeReg(NV_PRISCV_RISCV_CPUCTL, NV_PRISCV_CPUCTL_START);

    // Wait for core to become active
    for (int i = 0; i < 100; i++) {
        uint32_t status = readReg(NV_PRISCV_RISCV_CPUCTL);
        if (status & NV_PRISCV_CPUCTL_ACTIVE) {
            IOLog("NVDAAL-GSP: RISC-V core active\n");
            return true;
        }
        IODelay(1000);  // 1ms delay
    }

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
    size_t alignedSize = (elemSize + 0xFF) & ~0xFF;  // 256-byte aligned

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
    if (!gspReady && function != NV_VGPU_MSG_FUNCTION_GSP_SET_SYSTEM_INFO) {
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

bool NVDAALGsp::waitRpcResponse(uint32_t function, void *response, size_t responseSize, uint32_t timeoutMs) {
    // TODO: Implement proper status queue polling
    // For now, we assume the GSP processes commands quickly and rely on the status queue check in the main loop if needed
    // In a real implementation, we would wait for a specific completion entry in the Status Queue.
    // Since we are currently just firing and forgetting for initialization in this prototype phase:
    // IOLog("NVDAAL-GSP: waitRpcResponse not fully implemented\n");
    return true; 
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
    GspSystemInfo info;
    memset(&info, 0, sizeof(info));

    // Get PCI info from device
    if (pciDevice) {
        info.pciVendorId = pciDevice->configRead16(0x00);
        info.pciDeviceId = pciDevice->configRead16(0x02);
        info.pciSubVendorId = pciDevice->configRead16(0x2C);
        info.pciSubDeviceId = pciDevice->configRead16(0x2E);
        info.pciRevisionId = pciDevice->configRead8(0x08);

        // Get BAR addresses
        info.gpuPhysAddr = pciDevice->configRead32(0x10) & 0xFFFFFFF0;
        info.fbPhysAddr = pciDevice->configRead32(0x14) & 0xFFFFFFF0;
    }

    IOLog("NVDAAL-GSP: Sending system info (device 0x%04x)\n", info.pciDeviceId);

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
