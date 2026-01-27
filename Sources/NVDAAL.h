/*
 * NVDAAL.h - NVDAAL Driver Header
 */

#ifndef NVDAAL_H
#define NVDAAL_H

#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include "NVDAALGsp.h"
#include "NVDAALMemory.h"
#include "NVDAALChannel.h"
#include "NVDAALVASpace.h"
#include "NVDAALDisplay.h"

class NVDAAL : public IOService {
    OSDeclareDefaultStructors(NVDAAL);

private:
    // ... (existing members)
    IOPCIDevice *pciDevice;

    // Memory maps
    IOMemoryMap *bar0Map;      // MMIO registers (16MB)
    IOMemoryMap *bar1Map;      // VRAM aperture (24GB on 4090)

    // MMIO access
    volatile uint32_t *mmioBase;
    uint64_t mmioSize;

    // VRAM info
    uint64_t vramBase;
    uint64_t vramSize;

    // GPU info
    uint32_t chipId;
    uint32_t chipArch;
    uint32_t chipImpl;
    uint16_t deviceId;

    // GSP RM Handles
    uint32_t hClient;
    uint32_t hDevice;

    // Sub-components
    NVDAALGsp *gsp;
    NVDAALMemory *memory;
    NVDAALVASpace *vaSpace;
    NVDAALChannel *channel;
    NVDAALDisplay *display;

    // State
    bool computeReady;

private:
    // Hardware initialization
    bool mapBARs(void);
    void unmapBARs(void);
    bool identifyChip(void);
    bool initCompute(void);

    // Register access
    uint32_t readReg(uint32_t offset);
    void writeReg(uint32_t offset, uint32_t value);

    // Helper
    const char *getArchName(uint32_t arch);

public:
    // IOService lifecycle
    virtual bool init(OSDictionary *dictionary = nullptr) override;
    virtual void free(void) override;
    virtual IOService *probe(IOService *provider, SInt32 *score) override;
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;

    // User Client support
    virtual IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties, IOUserClient **handler) override;

    // Interface for User Client
    bool loadGspFirmware(const void *data, size_t size);
    uint64_t allocVram(size_t size);
    bool submitCommand(uint32_t cmd);
};

#endif // NVDAAL_H