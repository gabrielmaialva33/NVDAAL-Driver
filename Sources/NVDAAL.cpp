/*
 * NVDAAL.cpp - NVIDIA Ada Lovelace Compute Driver for macOS
 *
 * Open Source driver for RTX 4090 (AD102) - COMPUTE ONLY
 * No display support - focused on AI/ML workloads
 *
 * Part of the NVDAAL-Driver project
 * License: MIT
 */

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <libkern/libkern.h>
#include "NVDAALRegs.h"
#include "NVDAALUserClient.h"
#include "NVDAAL.h"

#define super IOService

OSDefineMetaClassAndStructors(NVDAAL, IOService);

// ============================================================================ 
// IOService Lifecycle
// ============================================================================ 

bool NVDAAL::init(OSDictionary *dictionary) {
    if (!super::init(dictionary)) {
        return false;
    }

    pciDevice = nullptr;
    bar0Map = nullptr;
    bar1Map = nullptr;
    mmioBase = nullptr;
    mmioSize = 0;
    vramBase = 0;
    vramSize = 0;
    chipId = 0;
    chipArch = 0;
    chipImpl = 0;
    deviceId = 0;
    gsp = nullptr;
    memory = nullptr;
    vaSpace = nullptr;
    channel = nullptr;
    display = nullptr;
    computeReady = false;
    
    hClient = 0;
    hDevice = 0;

    IOLog("NVDAAL: Compute driver initialized\n");
    return true;
}

void NVDAAL::free(void) {
    IOLog("NVDAAL: Driver freed\n");
    
    // Destroy GSP objects in reverse order logic usually happens in components
    // but here we just release the C++ objects
    if (channel) {
        channel->release();
        channel = nullptr;
    }
    if (vaSpace) {
        vaSpace->release();
        vaSpace = nullptr;
    }
    
    // We should free Client/Device handles too if GSP is still up
    if (gsp && hDevice) {
        gsp->rmFree(hClient, hClient, hDevice);
    }
    if (gsp && hClient) {
        gsp->rmFree(hClient, hClient, hClient);
    }

    if (gsp) {
        delete gsp;
        gsp = nullptr;
    }
    if (memory) {
        memory->release();
        memory = nullptr;
    }
    if (display) {
        display->release();
        display = nullptr;
    }
    super::free();
}

IOService *NVDAAL::probe(IOService *provider, SInt32 *score) {
    IOPCIDevice *device = OSDynamicCast(IOPCIDevice, provider);
    if (!device) {
        return nullptr;
    }

    // Check for NVIDIA vendor
    UInt32 vendorDevice = device->configRead32(0x00);
    UInt16 vendorID = vendorDevice & 0xFFFF;
    UInt16 devID = (vendorDevice >> 16) & 0xFFFF;

    if (vendorID != 0x10DE) {
        IOLog("NVDAAL: Not NVIDIA (vendor 0x%04x)\n", vendorID);
        return nullptr;
    }

    // Supported Ada Lovelace devices
    const char *deviceName = nullptr;
    switch (devID) {
        case 0x2684: deviceName = "RTX 4090"; break;
        case 0x2685: deviceName = "RTX 4090 D"; break;
        case 0x2702: deviceName = "RTX 4080 Super"; break;
        case 0x2704: deviceName = "RTX 4080"; break;
        case 0x2705: deviceName = "RTX 4070 Ti Super"; break;
        case 0x2782: deviceName = "RTX 4070 Ti"; break;
        case 0x2786: deviceName = "RTX 4070"; break;
        case 0x2860: deviceName = "RTX 4070 Super"; break;
        default:
            IOLog("NVDAAL: Unsupported device 0x%04x\n", devID);
            return nullptr;
    }

    IOLog("NVDAAL: Found %s (0x%04x) - Compute Mode\n", deviceName, devID);
    deviceId = devID;

    *score = 5000;  // High score to override Apple's stub drivers
    return this;
}

bool NVDAAL::start(IOService *provider) {
    if (!super::start(provider)) {
        return false;
    }

    pciDevice = OSDynamicCast(IOPCIDevice, provider);
    if (!pciDevice) {
        IOLog("NVDAAL: Failed to get PCI device\n");
        return false;
    }

    IOLog("NVDAAL: ========================================\n");
    IOLog("NVDAAL: Starting RTX 4090 Compute Driver\n");
    IOLog("NVDAAL: ========================================\n");

    // Enable PCI device
    pciDevice->setBusLeadEnable(true);
    pciDevice->setMemoryEnable(true);

    // Map BARs
    if (!mapBARs()) {
        IOLog("NVDAAL: Failed to map BARs\n");
        return false;
    }

    // Identify chip
    if (!identifyChip()) {
        IOLog("NVDAAL: Failed to identify chip\n");
        unmapBARs();
        return false;
    }

    // Initialize GSP (required for Ada Lovelace)
    IOLog("NVDAAL: Initializing GSP for %s...\n", getArchName(chipArch));
    
    gsp = new NVDAALGsp();
    if (!gsp || !gsp->init(pciDevice, mmioBase)) {
        IOLog("NVDAAL: Failed to create/init GSP controller\n");
        if (gsp) { delete gsp; gsp = nullptr; }
        unmapBARs();
        return false;
    }

    // Initialize Memory Manager
    memory = NVDAALMemory::withDevice(pciDevice, bar1Map);
    if (!memory) {
        IOLog("NVDAAL: Failed to initialize Memory Manager\n");
    }

    // Initialize Fake Display (Metal Spoofing)
    display = NVDAALDisplay::withDevice(pciDevice);
    if (display) {
        if (!display->attach(this) || !display->start(this)) {
            IOLog("NVDAAL: Failed to start Display Engine\n");
            display->release();
            display = nullptr;
        }
    }

    // Load and Boot GSP
    // Note: In a real scenario, firmware would be provided via UserClient or loaded from disk.
    // For now, we prepare the structures. The actual boot() will wait for firmware.
    
    // Read current status
    uint32_t wpr2 = readReg(NV_PFB_PRI_MMU_WPR2_ADDR_HI);
    IOLog("NVDAAL: WPR2 status: 0x%08x (enabled: %d)\n",
          wpr2, NV_PFB_WPR2_ENABLED(wpr2));

    if (NV_PFB_WPR2_ENABLED(wpr2)) {
        IOLog("NVDAAL: GSP might already be running or WPR is locked.\n");
    }

    IOLog("NVDAAL: ========================================\n");
    IOLog("NVDAAL: Driver started successfully\n");
    IOLog("NVDAAL: Awaiting firmware injection from user-space\n");
    IOLog("NVDAAL: ========================================\n");

    registerService();
    return true;
}

void NVDAAL::stop(IOService *provider) {
    IOLog("NVDAAL: Stopping compute driver\n");
    computeReady = false;
    unmapBARs();
    super::stop(provider);
}

// ============================================================================ 
// Hardware Initialization
// ============================================================================ 

bool NVDAAL::mapBARs(void) {
    if (!pciDevice) {
        return false;
    }

    // BAR0 - MMIO registers (~16-32MB)
    bar0Map = pciDevice->mapDeviceMemoryWithIndex(0);
    if (!bar0Map) {
        IOLog("NVDAAL: Failed to map BAR0 (MMIO)\n");
        return false;
    }

    mmioBase = (volatile uint32_t *)bar0Map->getVirtualAddress();
    mmioSize = bar0Map->getLength();

    IOLog("NVDAAL: BAR0 mapped @ %p (%llu MB)\n",
          (void *)mmioBase, mmioSize / (1024 * 1024));

    // BAR1 - VRAM aperture (up to 24GB on 4090)
    bar1Map = pciDevice->mapDeviceMemoryWithIndex(1);
    if (bar1Map) {
        vramBase = bar1Map->getVirtualAddress();
        vramSize = bar1Map->getLength();
        IOLog("NVDAAL: BAR1 mapped @ 0x%llx (%llu GB VRAM)\n",
              vramBase, vramSize / (1024 * 1024 * 1024));
    } else {
        IOLog("NVDAAL: BAR1 not mapped (will use indirect access)\n");
    }

    return true;
}

void NVDAAL::unmapBARs(void) {
    if (bar1Map) {
        bar1Map->release();
        bar1Map = nullptr;
    }

    if (bar0Map) {
        bar0Map->release();
        bar0Map = nullptr;
    }

    mmioBase = nullptr;
    vramBase = 0;
}

bool NVDAAL::identifyChip(void) {
    // Read chip identification registers
    chipId = readReg(NV_PMC_BOOT_0);
    uint32_t boot42 = readReg(NV_PMC_BOOT_42);

    // Extract architecture and implementation
    chipArch = (chipId >> 20) & 0x1F;
    chipImpl = (chipId >> 0) & 0xFF;

    IOLog("NVDAAL: BOOT0 = 0x%08x, BOOT42 = 0x%08x\n", chipId, boot42);
    IOLog("NVDAAL: Architecture: %s (0x%02x), Impl: 0x%02x\n",
          getArchName(chipArch), chipArch, chipImpl);

    // Verify it's Ada Lovelace
    if (chipArch != NV_CHIP_ARCH_ADA) {
        IOLog("NVDAAL: WARNING - Not Ada Lovelace architecture!\n");
        IOLog("NVDAAL: Expected 0x19 (Ada), got 0x%02x\n", chipArch);
        // Continue anyway for development
    }

    // Print VRAM info from config space
    uint32_t subsystem = pciDevice->configRead32(0x2C);
    IOLog("NVDAAL: Subsystem: 0x%08x\n", subsystem);

    return true;
}

const char *NVDAAL::getArchName(uint32_t arch) {
    switch (arch) {
        case 0x14: return "Maxwell";
        case 0x15: return "Pascal";
        case 0x16: return "Volta";
        case 0x17: return "Ampere";
        case 0x19: return "Ada Lovelace";
        case 0x1B: return "Blackwell";
        default: return "Unknown";
    }
}

bool NVDAAL::initCompute(void) {
    // TODO: Initialize compute queues
    IOLog("NVDAAL: Compute init not yet implemented\n");
    return false;
}

// ============================================================================ 
// User Client
// ============================================================================ 

IOReturn NVDAAL::newUserClient(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties, IOUserClient **handler) {
    if (!handler) return kIOReturnBadArgument;

    NVDAALUserClient *client = new NVDAALUserClient;
    if (!client) return kIOReturnNoMemory;

    if (!client->initWithTask(owningTask, securityID, type, properties)) {
        client->release();
        return kIOReturnInternalError;
    }

    if (!client->attach(this)) {
        client->release();
        return kIOReturnInternalError;
    }

    if (!client->start(this)) {
        client->detach(this);
        client->release();
        return kIOReturnInternalError;
    }

    *handler = client;
    return kIOReturnSuccess;
}

bool NVDAAL::loadGspFirmware(const void *data, size_t size) {
    if (!gsp) {
        IOLog("NVDAAL: GSP controller not available\n");
        return false;
    }

    IOLog("NVDAAL: Received GSP firmware (%lu bytes)\n", size);

    if (!gsp->parseElfFirmware(data, size)) {
        IOLog("NVDAAL: Failed to parse firmware ELF\n");
        return false;
    }
    
    if (!gsp->boot()) {
        IOLog("NVDAAL: Failed to boot GSP\n");
        return false;
    }

    if (!gsp->waitForInitDone()) {
        IOLog("NVDAAL: Timeout waiting for GSP init\n");
        return false;
    }

    IOLog("NVDAAL: GSP successfully initialized!\n");

    // ====================================================================
    // Initialize RM Hierarchy
    // ====================================================================
    
    // 1. Create Root Client
    hClient = gsp->nextHandle();
    Nv01RootClientParams clientParams = { 0 };
    if (!gsp->rmAlloc(hClient, NV01_NULL_OBJECT, hClient, NV01_ROOT_CLIENT, &clientParams, sizeof(clientParams))) {
        IOLog("NVDAAL: Failed to alloc Root Client\n");
        return false;
    }

    // 2. Create Device
    hDevice = gsp->nextHandle();
    // Device allocation params are usually simple class ID binding
    // For now we pass empty/null or minimal params if needed
    // Note: Some RM implementations require specific params for Device
    if (!gsp->rmAlloc(hClient, hClient, hDevice, AD102_COMPUTE_A, nullptr, 0)) {
        IOLog("NVDAAL: Failed to alloc Device (AD102_COMPUTE_A)\n");
        // return false; 
        // Proceeding carefully, might fail if params required
    }
    
    // 3. Initialize VASpace (MMU)
    vaSpace = NVDAALVASpace::withGsp(gsp, memory, hClient, hDevice);
    if (!vaSpace || !vaSpace->boot()) {
        IOLog("NVDAAL: Failed to boot VASpace\n");
        return false;
    }

    // 4. Initialize Compute Channel
    channel = NVDAALChannel::withVASpace(gsp, vaSpace, hClient, hDevice);
    if (!channel || !channel->boot()) {
        IOLog("NVDAAL: Failed to boot Compute Channel\n");
        return false;
    }

    computeReady = true;
    IOLog("NVDAAL: Compute Initialization COMPLETE!\n");

    return true;
}

uint64_t NVDAAL::allocVram(size_t size) {
    if (!memory) return 0;
    return memory->allocVram(size);
}

bool NVDAAL::submitCommand(uint32_t cmd) {
    if (!channel) return false;
    
    // TODO: cmd is currently a placeholder 32-bit value
    // Real submission requires a command buffer address and length
    // For now we just pass it as an address to test the plumbing
    return channel->submit((uint64_t)cmd, 4);
}

    

    // ============================================================================

    // Register Access

    
// ============================================================================ 

uint32_t NVDAAL::readReg(uint32_t offset) {
    if (!mmioBase) {
        return 0xFFFFFFFF;
    }
    return mmioBase[offset / 4];
}

void NVDAAL::writeReg(uint32_t offset, uint32_t value) {
    if (mmioBase) {
        mmioBase[offset / 4] = value;
        __sync_synchronize();
    }
}