/*
 * NVDAALVASpace.h - Virtual Address Space Management
 *
 * Manages GPU virtual memory page tables (MMU) for Ada Lovelace.
 * Handles the hierarchy of Page Directories (PDE) and Page Tables (PTE).
 */

#ifndef NVDAAL_VASPACE_H
#define NVDAAL_VASPACE_H

#include <IOKit/IOService.h>
#include "NVDAALGsp.h"
#include "NVDAALMemory.h"

class NVDAALVASpace : public OSObject {
    OSDeclareDefaultStructors(NVDAALVASpace);

private:
    NVDAALGsp *gsp;
    NVDAALMemory *memoryManager;
    
    // GSP RM Handles
    uint32_t hClient;
    uint32_t hDevice;
    uint32_t hVASpace; // The handle for this address space

    // Page Directory (Level 4/5 for Ada)
    // For simplicity in this prototype, we might start with a smaller structure
    // but Ada supports up to 5 levels.
    IOBufferMemoryDescriptor *pdeMem;
    uint64_t pdePhys;
    
    uint64_t vaStart;
    uint64_t vaLimit;
    uint64_t currentVaOffset; // Tracks the next free virtual address

public:
    static NVDAALVASpace* withGsp(NVDAALGsp *gsp, NVDAALMemory *mem, uint32_t hClient, uint32_t hDevice);
    
    virtual bool init() override;
    virtual void free() override;

    // Initialize the VASpace structure and register with GSP
    bool boot();

    // Map a physical memory descriptor into this VASpace
    // Returns the virtual address (GPU VA)
    uint64_t map(IOMemoryDescriptor *mem, uint64_t alignment = 0x1000);
    
    // Unmap
    void unmap(uint64_t va, size_t size);

    uint32_t getHandle() const { return hVASpace; }
    uint64_t getPdeAddress() const { return pdePhys; }
};

#endif // NVDAAL_VASPACE_H