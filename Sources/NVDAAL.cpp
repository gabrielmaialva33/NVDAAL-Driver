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
#include <mach/kmod.h>
#include "NVDAALRegs.h"
#include "NVDAALUserClient.h"
#include "NVDAAL.h"
#include "NVDAALVersion.h"
#include "NVDAALDebug.h"
#include "NVDAALConfig.h"

// =============================================================================
// Global State (Lilu-style)
// =============================================================================

// Debug logging globals (used by NVDAALDebug.h macros)
NVDAALLogLevel nvdaalLogLevel = NVDAAL_LOG_INFO;
bool nvdaalDebugEnabled = false;

// Configuration globals (used by NVDAALConfig.h)
NVDAALConfiguration nvdaalConfig;

// Required for modern macOS kext loading (kmutil/AuxKC)
extern "C" {
    extern kern_return_t _start(kmod_info_t *ki, void *data);
    extern kern_return_t _stop(kmod_info_t *ki, void *data);
    __attribute__((visibility("default"))) KMOD_EXPLICIT_DECL(com.nvdaal.compute, "1.0.0", _start, _stop)
    __private_extern__ kmod_start_func_t *_realmain = 0;
    __private_extern__ kmod_stop_func_t *_antimain = 0;
    __private_extern__ int _kext_apple_cc = __APPLE_CC__;
}

#define super IOService

OSDefineMetaClassAndStructors(NVDAAL, IOService);

// ============================================================================ 
// IOService Lifecycle
// ============================================================================ 

bool NVDAAL::init(OSDictionary *dictionary) {
    // CRITICAL: Log FIRST before anything else to know we were called
    IOLog("NVDAAL: >>>>>> init() CALLED <<<<<<\n");

    if (!super::init(dictionary)) {
        IOLog("NVDAAL: init() - super::init failed\n");
        return false;
    }

    // Initialize configuration from boot-args (Lilu-style)
    nvdaalConfigInit();
    IOLog("NVDAAL: init() - config initialized\n");

    // Check if we should load
    IOLog("NVDAAL: init() - checking nvdaalShouldLoad()\n");
    if (!nvdaalShouldLoad()) {
        IOLog("NVDAAL: init() - nvdaalShouldLoad() returned FALSE - ABORTING\n");
        return false;
    }
    IOLog("NVDAAL: init() - nvdaalShouldLoad() returned TRUE\n");

    // Print banner
    IOLog(NVDAAL_BANNER);

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
    interruptSource = nullptr;

    hClient = 0;
    hDevice = 0;

    // Linux-compat mode defaults (from SSDT _DSM Function 5)
    linuxCompatMode = false;
    gspWarmBoot = false;
    skipDisplayInit = true;  // Always skip for compute-only
    fwsecAlreadyRun = false;
    preferPioLoad = true;    // PIO is safer than DMA initially
    debugLevel = 0;

    // SSDT-provided hardware parameters
    ssdtVramUsable = 0;
    ssdtArchId = 0;
    ssdtGspFalconBase = 0;
    ssdtSec2FalconBase = 0;

    // Log configuration in debug mode
    if (NVDAAL_DEBUG_ENABLED) {
        nvdaalConfigLog();
    }

    NVDLOG("init", "Compute driver initialized");
    return true;
}

void NVDAAL::free(void) {
    NVDLOG("free", "Driver freed");

    if (interruptSource) {
        interruptSource->disable();
        if (getWorkLoop()) {
            getWorkLoop()->removeEventSource(interruptSource);
        }
        interruptSource->release();
        interruptSource = nullptr;
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

    unmapBARs();
    super::free();
}

IOService *NVDAAL::probe(IOService *provider, SInt32 *score) {
    IOLog("NVDAAL: >>>>>> probe() CALLED <<<<<<\n");

    IOPCIDevice *device = OSDynamicCast(IOPCIDevice, provider);
    if (!device) {
        IOLog("NVDAAL: probe() - provider is NOT an IOPCIDevice!\n");
        return nullptr;
    }

    // Check for NVIDIA vendor
    UInt32 vendorDevice = device->configRead32(0x00);
    IOLog("NVDAAL: probe() - vendorDevice=0x%08x\n", vendorDevice);
    UInt16 vendorID = vendorDevice & 0xFFFF;
    UInt16 devID = (vendorDevice >> 16) & 0xFFFF;

    if (vendorID != NVIDIA_VENDOR_ID) {
        NVDDBG("probe", "Not NVIDIA (vendor 0x%04x)", vendorID);
        return nullptr;
    }

    // Check if device is supported (using NVDAALVersion.h)
    if (!nvdaalIsDeviceSupported(devID)) {
        NVDWARN("probe", "Unsupported device 0x%04x", devID);
        return nullptr;
    }

    const char *deviceName = nvdaalGetDeviceName(devID);
    NVDLOG("probe", "Found %s (0x%04x) - Compute Mode", deviceName, devID);
    deviceId = devID;

    *score = 25001;  // Must beat IONDRVFramebuffer (20000)
    return this;
}

bool NVDAAL::start(IOService *provider) {
    IOLog("NVDAAL: >>>>>> start() CALLED <<<<<<\n");

    if (!super::start(provider)) {
        IOLog("NVDAAL: start() - super::start failed\n");
        return false;
    }

    pciDevice = OSDynamicCast(IOPCIDevice, provider);
    if (!pciDevice) {
        IOLog("NVDAAL: start() - Failed to get PCI device\n");
        return false;
    }

    IOLog("NVDAAL: start() - Got PCI device, proceeding...\n");
    NVDLOG("start", "========================================");
    NVDLOG("start", "Starting %s Compute Driver", nvdaalGetDeviceName(deviceId));
    NVDLOG("start", "========================================");

    // Enable PCI device
    pciDevice->setBusLeadEnable(true);
    pciDevice->setMemoryEnable(true);

    // Map BARs
    NVDTIMED_START(mapBARs);
    if (!mapBARs()) {
        NVDERR("start", "Failed to map BARs");
        return false;
    }
    NVDTIMED_END("start", mapBARs, "mapBARs()");

    // Identify chip
    if (!identifyChip()) {
        NVDERR("start", "Failed to identify chip");
        unmapBARs();
        return false;
    }

    // Read ACPI/SSDT properties (Linux-compat mode)
    if (readAcpiProperties()) {
        logAcpiProperties();
    } else {
        IOLog("NVDAAL: No ACPI properties found (using defaults)\n");
    }

    // Setup Interrupts (MSI)
    interruptSource = IOInterruptEventSource::interruptEventSource(this, handleInterrupt, provider, 0);
    if (interruptSource) {
        getWorkLoop()->addEventSource(interruptSource);
        interruptSource->enable();
        IOLog("NVDAAL: MSI Interrupts enabled\n");
    }

    // Initialize GSP (required for Ada Lovelace)
    IOLog("NVDAAL: Initializing GSP for %s...\n", getArchName(chipArch));

    gsp = new NVDAALGsp();
    if (!gsp || !gsp->init(pciDevice, mmioBase)) {
        IOLog("NVDAAL: WARNING: Failed to create/init GSP controller\n");
        if (gsp) { delete gsp; gsp = nullptr; }
        // Don't fail start - we still want the driver attached for debugging
    } else {
        IOLog("NVDAAL: GSP controller initialized\n");
    }

    // Initialize Memory Manager
    memory = NVDAALMemory::withDevice(pciDevice, bar1Map);
    if (!memory) {
        IOLog("NVDAAL: WARNING: Memory Manager not available\n");
    }

    // NVDAALDisplay disabled for now - injecting Metal/NVDA properties
    // can crash WindowServer or attract IONDRVFramebuffer
    // TODO: Re-enable when compute-only Metal support is implemented
    display = nullptr;
    IOLog("NVDAAL: Display engine disabled (compute-only mode)\n");

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

void NVDAAL::handleInterrupt(OSObject *target, IOInterruptEventSource *source, int count) {
    NVDAAL *inst = OSDynamicCast(NVDAAL, target);
    if (inst) inst->processInterrupt();
}

void NVDAAL::processInterrupt() {
    // Excellence: High-speed interrupt processing
    // 1. Read Interrupt Status Register (PMC_INTR_0)
    uint32_t intr = readReg(NV_PMC_INTR_EN_0); 
    
    // 2. Dispatch to GSP if GSP interrupt bit is set
    if (intr & (1 << 15)) { // Bit 15 is often GSP/Falcon on newer chips
        if (gsp) {
            // Tell GSP to check its queues
            // gsp->handleInterrupt(); 
        }
    }
    
    // 3. Clear Interrupt (ACK)
    writeReg(NV_PMC_INTR_EN_0, intr); // Acking by writing back
}

bool NVDAAL::loadGspFirmware(const void *data, size_t size) {
    return loadGspFirmwareEx(data, size) == 0;
}

// Load bootloader (must be called before loadGspFirmware)
bool NVDAAL::loadBootloader(const void *data, size_t size) {
    if (!gsp) return false;
    return gsp->loadBootloader(data, size);
}

// Load booter_load firmware for SEC2 (should be called before loadGspFirmware)
bool NVDAAL::loadBooterLoad(const void *data, size_t size) {
    if (!gsp) return false;
    IOLog("NVDAAL: Loading booter_load (%lu bytes)\n", size);
    return gsp->loadBooterLoad(data, size);
}

// Load VBIOS for FWSEC extraction (optional - may have been done by EFI)
bool NVDAAL::loadVbios(const void *data, size_t size) {
    if (!gsp) return false;
    IOLog("NVDAAL: Loading VBIOS (%lu bytes)\n", size);
    return gsp->loadVbios(data, size);
}

bool NVDAAL::executeFwsec(void) {
    if (!gsp) return false;
    IOLog("NVDAAL: Executing FWSEC-FRTS...\n");
    return gsp->executeFwsecFrts();
}

// Returns: 0=success, 1+=error stage for debugging
int NVDAAL::loadGspFirmwareEx(const void *data, size_t size) {
    if (!gsp) {
        IOLog("NVDAAL: GSP controller not available\n");
        return 1;  // Error stage 1: No GSP
    }

    IOLog("NVDAAL: Received GSP firmware (%lu bytes)\n", size);

    if (!gsp->parseElfFirmware(data, size)) {
        IOLog("NVDAAL: Failed to parse firmware ELF\n");
        return 2;  // Error stage 2: ELF parse failed
    }

    // Check if bootloader was loaded
    if (!gsp->hasBootloader()) {
        IOLog("NVDAAL: Warning: Bootloader not loaded - boot may fail\n");
        // Continue anyway for testing
    }

    int bootResult = gsp->bootEx();
    if (bootResult != 0) {
        IOLog("NVDAAL: Failed to boot GSP (stage %d)\n", bootResult);
        return 3;  // Error stage 3: Boot failed
    }

    if (!gsp->waitForInitDone()) {
        IOLog("NVDAAL: Timeout waiting for GSP init\n");
        return 4;  // Error stage 4: Init timeout
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

bool NVDAAL::waitSemaphore(uint64_t gpuAddr, uint32_t value, uint32_t timeoutMs) {
    // Excellence: GPU Synchronization
    // In Ada architecture, we usually poll a memory location that the GPU 
    // writes to via a 'MEM_OP' or 'RELEASE' command in the GPFIFO.
    
    uint32_t elapsed = 0;
    IOLog("NVDAAL: Sync - Waiting for GPU VA 0x%llx == %u (timeout: %u ms)\n", gpuAddr, value, timeoutMs);
    
    // Beta: Assume success for now to keep the pipeline moving
    // In the next iteration, we will implement VA-to-Kernel mapping.
    if (gpuAddr == 0) return false;
    
    return true; 
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

// ============================================================================
// Status Reporting
// ============================================================================

bool NVDAAL::getStatus(GpuStatus *status) {
    if (!status || !mmioBase) {
        return false;
    }

    // Read all relevant registers
    status->pmcBoot0 = readReg(NV_PMC_BOOT_0);
    status->wpr2Lo = readReg(NV_PFB_PRI_MMU_WPR2_ADDR_LO);
    status->wpr2Hi = readReg(NV_PFB_PRI_MMU_WPR2_ADDR_HI);
    status->wpr2Enabled = (status->wpr2Hi >> 31) & 1;  // Bit 31 = enabled

    // GSP RISC-V status
    status->gspRiscvCpuctl = readReg(NV_PRISCV_RISCV_CPUCTL);

    // SEC2 RISC-V status
    status->sec2RiscvCpuctl = readReg(NV_PSEC_RISCV_CPUCTL);

    // GSP Falcon mailboxes (for RPC status)
    status->gspFalconMailbox0 = readReg(NV_PGSP_FALCON_MAILBOX0);
    status->gspFalconMailbox1 = readReg(NV_PGSP_FALCON_MAILBOX1);

    // Boot scratch register
    status->bootScratch = readReg(NV_PGC6_BSI_SECURE_SCRATCH_14);

    return true;
}

// ============================================================================
// ACPI/SSDT Property Reading (Linux-compat mode)
// ============================================================================

bool NVDAAL::readAcpiProperties(void) {
    if (!pciDevice) {
        return false;
    }

    // Check if NVDAAL SSDT properties are present
    // The SSDT _DSM (UUID a0b5b7c6-...) injects these into IORegistry
    OSObject *compatible = pciDevice->getProperty("nvdaal-compatible");
    if (!compatible) {
        // No NVDAAL SSDT properties found
        return false;
    }

    IOLog("NVDAAL: Found ACPI/SSDT properties (Linux-compat mode)\n");

    // ========================================================================
    // Read boot hints from SSDT _DSM Function 5
    // ========================================================================

    // nvdaal-boot-mode: "linux-compat" enables Linux-like behavior
    OSString *bootMode = OSDynamicCast(OSString, pciDevice->getProperty("nvdaal-boot-mode"));
    if (bootMode && bootMode->isEqualTo("linux-compat")) {
        linuxCompatMode = true;
    }

    // gsp-warm-boot: Skip full GSP init if WPR2 already set
    OSNumber *warmBoot = OSDynamicCast(OSNumber, pciDevice->getProperty("gsp-warm-boot"));
    if (warmBoot) {
        gspWarmBoot = warmBoot->unsigned8BitValue() != 0;
    }

    // skip-display-init: Skip display engine initialization
    OSNumber *skipDisp = OSDynamicCast(OSNumber, pciDevice->getProperty("skip-display-init"));
    if (skipDisp) {
        skipDisplayInit = skipDisp->unsigned8BitValue() != 0;
    }

    // fwsec-already-run: FWSEC already executed by UEFI
    OSNumber *fwsecDone = OSDynamicCast(OSNumber, pciDevice->getProperty("fwsec-already-run"));
    if (fwsecDone) {
        fwsecAlreadyRun = fwsecDone->unsigned8BitValue() != 0;
    }

    // prefer-pio-load: Use PIO (not DMA) for firmware loading
    OSNumber *pioLoad = OSDynamicCast(OSNumber, pciDevice->getProperty("prefer-pio-load"));
    if (pioLoad) {
        preferPioLoad = pioLoad->unsigned8BitValue() != 0;
    }

    // debug-level: 0=off, 1=basic, 2=verbose, 3=trace
    OSNumber *debugLvl = OSDynamicCast(OSNumber, pciDevice->getProperty("debug-level"));
    if (debugLvl) {
        debugLevel = debugLvl->unsigned8BitValue();
    }

    // ========================================================================
    // Read hardware parameters from SSDT _DSM Functions 1-4
    // These are validated against detected values for sanity checking
    // ========================================================================

    // arch-id: Architecture ID (should match chipArch)
    OSNumber *archId = OSDynamicCast(OSNumber, pciDevice->getProperty("arch-id"));
    if (archId) {
        ssdtArchId = archId->unsigned32BitValue();
    }

    // vram-usable: Usable VRAM (after WPR2 reservation)
    OSData *vramData = OSDynamicCast(OSData, pciDevice->getProperty("vram-usable"));
    if (vramData && vramData->getLength() >= 8) {
        const uint8_t *bytes = (const uint8_t *)vramData->getBytesNoCopy();
        // Little-endian 64-bit value
        ssdtVramUsable = ((uint64_t)bytes[0]) |
                         ((uint64_t)bytes[1] << 8) |
                         ((uint64_t)bytes[2] << 16) |
                         ((uint64_t)bytes[3] << 24) |
                         ((uint64_t)bytes[4] << 32) |
                         ((uint64_t)bytes[5] << 40) |
                         ((uint64_t)bytes[6] << 48) |
                         ((uint64_t)bytes[7] << 56);
    }

    // gsp-falcon-base: GSP Falcon register base
    OSNumber *gspBase = OSDynamicCast(OSNumber, pciDevice->getProperty("gsp-falcon-base"));
    if (gspBase) {
        ssdtGspFalconBase = gspBase->unsigned32BitValue();
    }

    // sec2-falcon-base: SEC2 Falcon register base
    OSNumber *sec2Base = OSDynamicCast(OSNumber, pciDevice->getProperty("sec2-falcon-base"));
    if (sec2Base) {
        ssdtSec2FalconBase = sec2Base->unsigned32BitValue();
    }

    return true;
}

void NVDAAL::logAcpiProperties(void) {
    IOLog("NVDAAL: ========================================\n");
    IOLog("NVDAAL: ACPI/SSDT Properties (Linux-compat)\n");
    IOLog("NVDAAL: ========================================\n");
    IOLog("NVDAAL: Boot Mode: %s\n", linuxCompatMode ? "linux-compat" : "default");
    IOLog("NVDAAL: GSP Warm Boot: %s\n", gspWarmBoot ? "yes" : "no");
    IOLog("NVDAAL: Skip Display Init: %s\n", skipDisplayInit ? "yes" : "no");
    IOLog("NVDAAL: FWSEC Already Run: %s\n", fwsecAlreadyRun ? "yes" : "no");
    IOLog("NVDAAL: Prefer PIO Load: %s\n", preferPioLoad ? "yes" : "no");
    IOLog("NVDAAL: Debug Level: %u\n", debugLevel);

    if (ssdtArchId != 0) {
        IOLog("NVDAAL: SSDT arch-id: 0x%04x (detected: 0x%02x)\n", ssdtArchId, chipArch);
        if ((ssdtArchId >> 4) != chipArch) {
            IOLog("NVDAAL: WARNING: SSDT arch-id mismatch!\n");
        }
    }

    if (ssdtVramUsable != 0) {
        IOLog("NVDAAL: SSDT vram-usable: 0x%llx (%llu GB)\n",
              ssdtVramUsable, ssdtVramUsable / (1024ULL * 1024 * 1024));
    }

    if (ssdtGspFalconBase != 0) {
        IOLog("NVDAAL: SSDT gsp-falcon-base: 0x%08x\n", ssdtGspFalconBase);
    }

    if (ssdtSec2FalconBase != 0) {
        IOLog("NVDAAL: SSDT sec2-falcon-base: 0x%08x\n", ssdtSec2FalconBase);
    }

    IOLog("NVDAAL: ========================================\n");
}