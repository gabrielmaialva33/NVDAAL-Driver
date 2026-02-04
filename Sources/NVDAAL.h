/*
 * NVDAAL.h - NVDAAL Driver Header
 */

#ifndef NVDAAL_H
#define NVDAAL_H

#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOInterruptEventSource.h>
#include "NVDAALGsp.h"
#include "NVDAALMemory.h"
#include "NVDAALChannel.h"
#include "NVDAALVASpace.h"
#include "NVDAALDisplay.h"

class NVDAAL : public IOService {
    OSDeclareDefaultStructors(NVDAAL);

private:
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

    // Interrupts
    IOInterruptEventSource *interruptSource;
    static void handleInterrupt(OSObject *target, IOInterruptEventSource *source, int count);
    void processInterrupt();

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
    int loadGspFirmwareEx(const void *data, size_t size);  // Returns error stage (0=success)
    bool loadBootloader(const void *data, size_t size);
    bool loadBooterLoad(const void *data, size_t size);    // SEC2 booter firmware
    bool loadVbios(const void *data, size_t size);         // VBIOS for FWSEC
    bool executeFwsec(void);                               // Execute FWSEC-FRTS to configure WPR2
    uint64_t allocVram(size_t size);
    bool submitCommand(uint32_t cmd);
    bool waitSemaphore(uint64_t gpuAddr, uint32_t value, uint32_t timeoutMs);

    // Status reporting (for debugging WPR2/GSP state)
    struct GpuStatus {
        uint32_t pmcBoot0;           // Chip ID
        uint32_t wpr2Lo;             // WPR2 low address
        uint32_t wpr2Hi;             // WPR2 high address
        bool     wpr2Enabled;        // WPR2 active flag
        uint32_t gspRiscvCpuctl;     // GSP RISC-V CPUCTL
        uint32_t sec2RiscvCpuctl;    // SEC2 RISC-V CPUCTL
        uint32_t gspFalconMailbox0;  // GSP Falcon mailbox
        uint32_t gspFalconMailbox1;
        uint32_t bootScratch;        // Boot stage scratch register
    };
    bool getStatus(GpuStatus *status);

    // IOKit Register Access Overrides (Internal)
    virtual bool setProperty(const OSSymbol *aKey, OSObject *anObject) override;
};

#endif // NVDAAL_H
