/*
 * NVDAALChannel.h - Compute Channel (GPFIFO)
 *
 * Implements a hardware channel for submitting work to the GPU.
 * Uses the GSP RM hierarchy: Client -> Device -> SubDevice -> Channel.
 */

#ifndef NVDAAL_CHANNEL_H
#define NVDAAL_CHANNEL_H

#include <IOKit/IOService.h>
#include "NVDAALGsp.h"
#include "NVDAALVASpace.h"

class NVDAALChannel : public OSObject {
    OSDeclareDefaultStructors(NVDAALChannel);

private:
    NVDAALGsp *gsp;
    NVDAALVASpace *vaSpace;
    
    // RM Handles
    uint32_t hClient;
    uint32_t hDevice;
    uint32_t hSubDevice;
    uint32_t hChannel;

    // Hardware GPFIFO Entry Format (16 bytes)
    struct NvGpfifoEntry {
        uint64_t address; // GPU Virtual Address of PushBuffer
        uint32_t length;  // Length in bytes
        uint32_t flags;   // Flags (bit 0 = fetch trigger)
    };

    // GPFIFO Ring Buffer (Kernel side)
    IOBufferMemoryDescriptor *gpfifoMem;
    uint64_t gpfifoPhys;
    volatile NvGpfifoEntry *gpfifoRing; // Updated to use proper struct
    uint32_t ringSize;
    uint32_t put;
    uint32_t get;

    // User Doorbell (UserD)
    IOBufferMemoryDescriptor *userdMem;
    uint64_t userdPhys;
    volatile uint32_t *userd;

    IOLock *lock;

public:
    static NVDAALChannel* withVASpace(NVDAALGsp *gsp, NVDAALVASpace *vaSpace, uint32_t hClient, uint32_t hDevice);

    virtual bool init() override;
    virtual void free() override;

    // Create the channel object via GSP RPC
    bool boot();

    // Submit work (PushBuffer) to the channel
    bool submit(uint64_t pbGpuAddr, uint32_t pbLength);

    uint32_t getHandle() const { return hChannel; }
};

#endif // NVDAAL_CHANNEL_H