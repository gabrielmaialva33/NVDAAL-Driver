/*
 * NVDAALUserClient.cpp - IOUserClient Implementation
 */

#include "NVDAALUserClient.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>

#define super IOUserClient

OSDefineMetaClassAndStructors(NVDAALUserClient, IOUserClient);

// =============================================================================
// Lifecycle
// ============================================================================n
bool NVDAALUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties) {
    if (!super::initWithTask(owningTask, securityID, type, properties)) {
        return false;
    }
    clientTask = owningTask;
    return true;
}

bool NVDAALUserClient::start(IOService *service) {
    if (!super::start(service)) {
        return false;
    }
    
    this->provider = OSDynamicCast(NVDAAL, service);
    if (!this->provider) {
        return false;
    }

    return true;
}

void NVDAALUserClient::stop(IOService *service) {
    super::stop(service);
    this->provider = nullptr;
}

IOReturn NVDAALUserClient::clientClose(void) {
    terminate();
    return kIOReturnSuccess;
}

// ============================================================================n// External Methods
// ============================================================================n

IOReturn NVDAALUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *arguments,
                                          IOExternalMethodDispatch *dispatch, OSObject *target, void *reference) {
    switch (selector) {
        case kNVDAALMethodLoadFirmware:
            return methodLoadFirmware(arguments);
        case kNVDAALMethodAllocVram:
            return methodAllocVram(arguments);
        case kNVDAALMethodSubmitCommand:
            return methodSubmitCommand(arguments);
        case kNVDAALMethodWaitSemaphore:
            return methodWaitSemaphore(arguments);
        case kNVDAALMethodLoadBooterLoad:
            return methodLoadBooterLoad(arguments);
        case kNVDAALMethodLoadVbios:
            return methodLoadVbios(arguments);
        case kNVDAALMethodLoadBootloader:
            return methodLoadBootloader(arguments);
        case kNVDAALMethodGetStatus:
            return methodGetStatus(arguments);
        default:
            return kIOReturnBadArgument;
    }
}

IOReturn NVDAALUserClient::methodWaitSemaphore(IOExternalMethodArguments *args) {
    if (args->scalarInputCount != 2) return kIOReturnBadArgument;
    return provider->waitSemaphore(args->scalarInput[0], (uint32_t)args->scalarInput[1], 1000) ? kIOReturnSuccess : kIOReturnTimeout;
}

IOReturn NVDAALUserClient::methodAllocVram(IOExternalMethodArguments *args) {
    if (args->scalarInputCount != 1 || args->scalarOutputCount != 1) {
        return kIOReturnBadArgument;
    }

    size_t size = (size_t)args->scalarInput[0];
    uint64_t offset = provider->allocVram(size);

    if (offset == 0 && size > 0) return kIOReturnNoMemory;

    args->scalarOutput[0] = offset;
    return kIOReturnSuccess;
}

IOReturn NVDAALUserClient::methodSubmitCommand(IOExternalMethodArguments *args) {
    if (args->scalarInputCount != 1) {
        return kIOReturnBadArgument;
    }

    uint32_t cmd = (uint32_t)args->scalarInput[0];
    bool ok = provider->submitCommand(cmd);

    return ok ? kIOReturnSuccess : kIOReturnError;
}

IOReturn NVDAALUserClient::methodLoadFirmware(IOExternalMethodArguments *args) {
    // Expects:
    // Input[0]: Pointer to GSP firmware (user virtual address)
    // Input[1]: Size of GSP firmware
    
    if (args->scalarInputCount != 2) {
        return kIOReturnBadArgument;
    }

    mach_vm_address_t userPtr = (mach_vm_address_t)args->scalarInput[0];
    mach_vm_size_t size = (mach_vm_size_t)args->scalarInput[1];

    IOLog("NVDAALUserClient: LoadFirmware called. Ptr: 0x%llx, Size: %llu\n", userPtr, size);

    // Validate size (sanity check, e.g. < 256MB)
    if (size == 0 || size > 0x10000000) {
        IOLog("NVDAALUserClient: Invalid firmware size\n");
        return kIOReturnBadArgument;
    }

    // Create memory descriptor from user memory
    IOMemoryDescriptor *memDesc = IOMemoryDescriptor::withAddressRange(
        userPtr,
        size,
        kIODirectionOut,
        clientTask
    );

    if (!memDesc) {
        IOLog("NVDAALUserClient: Failed to create memory descriptor\n");
        return kIOReturnNoMemory;
    }

    // Wire down the memory (prepare)
    IOReturn ret = memDesc->prepare(kIODirectionOut);
    if (ret != kIOReturnSuccess) {
        IOLog("NVDAALUserClient: Failed to wire memory (0x%08x)\n", ret);
        memDesc->release();
        return ret;
    }

    // Get physical address/map kernel side
    // For GSP, we likely need the physical address to pass to the GPU via DMA
    // We can also map it to kernel virtual address if we need to parse it (ELF)
    
    IOMemoryMap *map = memDesc->map();
    if (!map) {
        IOLog("NVDAALUserClient: Failed to map memory to kernel\n");
        memDesc->complete(kIODirectionOut);
        memDesc->release();
        return kIOReturnVMError;
    }

    void *kernelAddr = (void *)map->getVirtualAddress();
    
    // Call driver to load firmware
    // Note: provider->loadGspFirmware needs to be implemented publicly in NVDAAL
    int result = provider->loadGspFirmwareEx(kernelAddr, size);

    // Cleanup
    map->release();
    memDesc->complete(kIODirectionOut);
    memDesc->release();

    // Return specific error code from loadGspFirmwareEx
    // 0 = success, 1+ = error stage
    if (result == 0) return kIOReturnSuccess;
    // Encode error stage in return code: 0xe0000300 + stage
    return (IOReturn)(0xe0000300 + result);
}

IOReturn NVDAALUserClient::methodLoadBooterLoad(IOExternalMethodArguments *args) {
    // Load booter_load firmware for SEC2
    // Input[0]: Pointer to booter_load data
    // Input[1]: Size

    if (args->scalarInputCount != 2) {
        return kIOReturnBadArgument;
    }

    mach_vm_address_t userPtr = (mach_vm_address_t)args->scalarInput[0];
    mach_vm_size_t size = (mach_vm_size_t)args->scalarInput[1];

    IOLog("NVDAALUserClient: LoadBooterLoad. Ptr: 0x%llx, Size: %llu\n", userPtr, size);

    if (size == 0 || size > 0x100000) {  // Max 1MB for booter
        return kIOReturnBadArgument;
    }

    IOMemoryDescriptor *memDesc = IOMemoryDescriptor::withAddressRange(
        userPtr, size, kIODirectionOut, clientTask);
    if (!memDesc) return kIOReturnNoMemory;

    IOReturn ret = memDesc->prepare(kIODirectionOut);
    if (ret != kIOReturnSuccess) {
        memDesc->release();
        return ret;
    }

    IOMemoryMap *map = memDesc->map();
    if (!map) {
        memDesc->complete(kIODirectionOut);
        memDesc->release();
        return kIOReturnVMError;
    }

    void *kernelAddr = (void *)map->getVirtualAddress();
    bool ok = provider->loadBooterLoad(kernelAddr, size);

    map->release();
    memDesc->complete(kIODirectionOut);
    memDesc->release();

    return ok ? kIOReturnSuccess : kIOReturnError;
}

IOReturn NVDAALUserClient::methodLoadVbios(IOExternalMethodArguments *args) {
    // Load VBIOS for FWSEC extraction
    // Input[0]: Pointer to VBIOS data
    // Input[1]: Size

    if (args->scalarInputCount != 2) {
        return kIOReturnBadArgument;
    }

    mach_vm_address_t userPtr = (mach_vm_address_t)args->scalarInput[0];
    mach_vm_size_t size = (mach_vm_size_t)args->scalarInput[1];

    IOLog("NVDAALUserClient: LoadVbios. Ptr: 0x%llx, Size: %llu\n", userPtr, size);

    if (size == 0 || size > 0x400000) {  // Max 4MB for VBIOS
        return kIOReturnBadArgument;
    }

    IOMemoryDescriptor *memDesc = IOMemoryDescriptor::withAddressRange(
        userPtr, size, kIODirectionOut, clientTask);
    if (!memDesc) return kIOReturnNoMemory;

    IOReturn ret = memDesc->prepare(kIODirectionOut);
    if (ret != kIOReturnSuccess) {
        memDesc->release();
        return ret;
    }

    IOMemoryMap *map = memDesc->map();
    if (!map) {
        memDesc->complete(kIODirectionOut);
        memDesc->release();
        return kIOReturnVMError;
    }

    void *kernelAddr = (void *)map->getVirtualAddress();
    bool ok = provider->loadVbios(kernelAddr, size);

    map->release();
    memDesc->complete(kIODirectionOut);
    memDesc->release();

    return ok ? kIOReturnSuccess : kIOReturnError;
}

IOReturn NVDAALUserClient::methodLoadBootloader(IOExternalMethodArguments *args) {
    // Load GSP bootloader firmware (bootloader-ad102-570.144.bin)
    // Input[0]: Pointer to bootloader data
    // Input[1]: Size

    if (args->scalarInputCount != 2) {
        return kIOReturnBadArgument;
    }

    mach_vm_address_t userPtr = (mach_vm_address_t)args->scalarInput[0];
    mach_vm_size_t size = (mach_vm_size_t)args->scalarInput[1];

    IOLog("NVDAALUserClient: LoadBootloader. Ptr: 0x%llx, Size: %llu\n", userPtr, size);

    if (size == 0 || size > 0x100000) {  // Max 1MB for bootloader
        return kIOReturnBadArgument;
    }

    IOMemoryDescriptor *memDesc = IOMemoryDescriptor::withAddressRange(
        userPtr, size, kIODirectionOut, clientTask);
    if (!memDesc) return kIOReturnNoMemory;

    IOReturn ret = memDesc->prepare(kIODirectionOut);
    if (ret != kIOReturnSuccess) {
        memDesc->release();
        return ret;
    }

    IOMemoryMap *map = memDesc->map();
    if (!map) {
        memDesc->complete(kIODirectionOut);
        memDesc->release();
        return kIOReturnVMError;
    }

    void *kernelAddr = (void *)map->getVirtualAddress();
    bool ok = provider->loadBootloader(kernelAddr, size);

    map->release();
    memDesc->complete(kIODirectionOut);
    memDesc->release();

    return ok ? kIOReturnSuccess : kIOReturnError;
}

IOReturn NVDAALUserClient::methodGetStatus(IOExternalMethodArguments *args) {
    // Returns GPU status registers via structureOutput
    // Expected: structureOutputSize >= sizeof(NVDAAL::GpuStatus)

    if (!args->structureOutput || args->structureOutputSize < sizeof(NVDAAL::GpuStatus)) {
        return kIOReturnBadArgument;
    }

    NVDAAL::GpuStatus status;
    if (!provider->getStatus(&status)) {
        return kIOReturnError;
    }

    memcpy(args->structureOutput, &status, sizeof(NVDAAL::GpuStatus));
    return kIOReturnSuccess;
}
