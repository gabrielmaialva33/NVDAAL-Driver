/*
 * NVDAALChannel.cpp - Compute Channel Implementation
 */

#include "NVDAALChannel.h"
#include "NVDAALRegs.h"
#include <IOKit/IOLib.h>

#define super OSObject

OSDefineMetaClassAndStructors(NVDAALChannel, OSObject);

NVDAALChannel* NVDAALChannel::withVASpace(NVDAALGsp *gsp, NVDAALVASpace *vaSpace, uint32_t hClient, uint32_t hDevice) {
    NVDAALChannel *inst = new NVDAALChannel;
    if (inst) {
        inst->gsp = gsp;
        inst->vaSpace = vaSpace;
        inst->hClient = hClient;
        inst->hDevice = hDevice;
        if (!inst->init()) {
            inst->release();
            return nullptr;
        }
    }
    return inst;
}

bool NVDAALChannel::init() {
    if (!super::init()) return false;
    
    ringSize = 0x1000; // 4096 entries
    put = 0;
    get = 0;
    
    lock = IOLockAlloc();
    if (!lock) return false;
    
    return true;
}

void NVDAALChannel::free() {
    if (hChannel) {
        gsp->rmFree(hClient, hSubDevice, hChannel);
    }
    if (hSubDevice) {
        gsp->rmFree(hClient, hDevice, hSubDevice);
    }
    
    // Free buffers
    if (gpfifoMem) {
        gpfifoMem->complete();
        gpfifoMem->release();
    }
    if (userdMem) {
        userdMem->complete();
        userdMem->release();
    }
    if (lock) IOLockFree(lock);
    
    super::free();
}

bool NVDAALChannel::boot() {
    if (!gsp || !vaSpace) return false;

    IOLog("NVDAAL-Channel: Booting Compute Channel...\n");

    // 1. Allocate SubDevice
    hSubDevice = gsp->nextHandle();
    // SubDevice usually doesn't need params for simple creation under Device
    if (!gsp->rmAlloc(hClient, hDevice, hSubDevice, GF100_SUBDEVICE_FULL, nullptr, 0)) {
        IOLog("NVDAAL-Channel: Failed to allocate SubDevice\n");
        return false;
    }

    // 2. Allocate GPFIFO Ring
    // Entry size is 16 bytes (Address + Length + Flags)
    gpfifoMem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, 
        kIODirectionInOut | kIOMemoryPhysicallyContiguous,
        ringSize * sizeof(NvGpfifoEntry), 
        0xFFFFFFFFFFFFULL
    );
    if (!gpfifoMem || gpfifoMem->prepare() != kIOReturnSuccess) return false;
    gpfifoPhys = gpfifoMem->getPhysicalSegment(0, nullptr);
    gpfifoRing = (volatile NvGpfifoEntry *)gpfifoMem->getBytesNoCopy();
    memset((void *)gpfifoRing, 0, ringSize * sizeof(NvGpfifoEntry));

    // 3. Allocate UserD (Doorbell)
    userdMem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task,
        kIODirectionInOut | kIOMemoryPhysicallyContiguous,
        0x1000, // 4KB page
        0xFFFFFFFFFFFFULL
    );
    if (!userdMem || userdMem->prepare() != kIOReturnSuccess) return false;
    userdPhys = userdMem->getPhysicalSegment(0, nullptr);
    userd = (volatile uint32_t *)userdMem->getBytesNoCopy();
    memset((void *)userd, 0, 0x1000);

    // 4. Register UserD Memory with GSP (Need a memory handle)
    uint32_t hUserdMem = gsp->nextHandle();
    NvMemoryAllocParams memParams;
    memset(&memParams, 0, sizeof(memParams));
    memParams.type = NV01_MEMORY_SYSTEM; // GTT
    memParams.size = 0x1000;
    memParams.address = userdPhys; // Physical address backing
    
    // Note: For system memory, we typically use NV01_MEMORY_SYSTEM class
    // In a full driver, we'd need a more complex memory registration (Memory Fabric)
    // but for prototype, we try direct registration.
    if (!gsp->rmAlloc(hClient, hDevice, hUserdMem, NV01_MEMORY_SYSTEM, &memParams, sizeof(memParams))) {
        IOLog("NVDAAL-Channel: Failed to register UserD memory\n");
        // return false; // Non-fatal for now while debugging RM classes
    }

    // 5. Create Channel
    hChannel = gsp->nextHandle();
    NvChannelAllocParams chanParams;
    memset(&chanParams, 0, sizeof(chanParams));
    chanParams.ampMode = 1; // Ampere+
    chanParams.engineType = NV2080_ENGINE_TYPE_COMPUTE;
    chanParams.gpFifoOffset = 0; // We will use manual put/get or update via UserD
    chanParams.gpFifoEntries = ringSize;
    chanParams.flags = 0;
    chanParams.hUserdMemory = hUserdMem;
    chanParams.userdOffset = 0;

    // IMPORTANT: The parent of a Channel in RM is usually the VASpace or SubDevice+VASpace
    // NVIDIA RM topology: Device -> ChannelGroup -> Channel
    // For simple flat topology often used in basics: Device -> Channel
    // We try allocating under SubDevice, passing VASpace as a parameter if needed (via separate bind)
    // or sometimes Channel is child of VASpace. Let's try SubDevice.
    
    if (!gsp->rmAlloc(hClient, hSubDevice, hChannel, ADA_CHANNEL_GPFIFO_A, &chanParams, sizeof(chanParams))) {
        IOLog("NVDAAL-Channel: Failed to allocate GPFIFO Channel\n");
        return false;
    }

    IOLog("NVDAAL-Channel: Channel created (Handle: 0x%x)\n", hChannel);
    return true;
}

bool NVDAALChannel::submit(uint64_t pbGpuAddr, uint32_t pbLength) {
    IOLockLock(lock);
    
    // Format the GPFIFO Entry (Excellence!)
    // Address must be aligned? Usually yes.
    gpfifoRing[put].address = pbGpuAddr;
    gpfifoRing[put].length = pbLength;
    gpfifoRing[put].flags = 1; // Trigger fetch (GPFIFO_ENTRY_FLAG_FETCH)
    
    // Memory Barrier to ensure entry is written before doorbell
    __sync_synchronize();

    put = (put + 1) % ringSize;
    
    // Ring the doorbell
    // Write the new PUT value to the UserD or GSP register
    if (userd) {
        userd[0] = put; // Offset 0 is usually Put
        __sync_synchronize();
    }
    
    IOLockUnlock(lock);
    return true;
}
