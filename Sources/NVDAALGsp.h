/*
 * NVDAALGsp.h - GSP (GPU System Processor) Controller
 *
 * Handles:
 * - GSP firmware loading
 * - RPC communication
 * - Boot sequence for Ada Lovelace
 *
 * Based on TinyGPU but rewritten for IOKit/macOS
 */

#ifndef NVDAAL_GSP_H
#define NVDAAL_GSP_H

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/pci/IOPCIDevice.h>
#include "NVDAALRegs.h"

// ============================================================================
// ELF Definitions
// ============================================================================

#pragma pack(push, 1)

typedef struct {
    uint8_t  ident[16];      // Magic number and other info
    uint16_t type;           // Object file type
    uint16_t machine;        // Architecture
    uint32_t version;        // Object file version
    uint64_t entry;          // Entry point virtual address
    uint64_t phoff;          // Program header table file offset
    uint64_t shoff;          // Section header table file offset
    uint32_t flags;          // Processor-specific flags
    uint16_t ehsize;         // ELF header size in bytes
    uint16_t phentsize;      // Program header table entry size
    uint16_t phnum;          // Program header table entry count
    uint16_t shentsize;      // Section header table entry size
    uint16_t shnum;          // Section header table entry count
    uint16_t shstrndx;       // Section header string table index
} Elf64_Ehdr;

typedef struct {
    uint32_t name;           // Section name (string tbl index)
    uint32_t type;           // Section type
    uint64_t flags;          // Section flags
    uint64_t addr;           // Section virtual addr at execution
    uint64_t offset;         // Section file offset
    uint64_t size;           // Section size in bytes
    uint32_t link;           // Link to another section
    uint32_t info;           // Additional section information
    uint64_t addralign;      // Section alignment
    uint64_t entsize;        // Entry size if section holds table
} Elf64_Shdr;

#pragma pack(pop)

class NVDAALGsp {
public:
    NVDAALGsp(void);
    ~NVDAALGsp(void);

    // Initialization
    bool init(IOPCIDevice *pciDevice, volatile uint32_t *mmio);
    void free(void);

    // Firmware loading
    bool loadFirmware(const char *firmwarePath);
    bool loadBootloader(const void *data, size_t size);
    bool parseElfFirmware(const void *data, size_t size);

    // Boot sequence
    bool boot(void);
    bool waitForInitDone(uint32_t timeoutMs = 5000);
    uint32_t getBootStatus(void) const;

    // RPC primitives
    bool sendRpc(uint32_t function, const void *params, size_t size);
    bool waitRpcResponse(uint32_t function, void *response, size_t responseSize, uint32_t timeoutMs = 1000);
    
    // Higher level RPC helpers
    bool sendSystemInfo(void);
    bool setRegistry(const char *key, uint32_t value);

    // Resource Manager (RM) Interface
    // Used to create/manage GSP objects (Client -> Device -> SubDevice -> etc)
    bool rmAlloc(uint32_t hClient, uint32_t hParent, uint32_t hObject, uint32_t hClass, void *params, size_t paramsSize);
    bool rmControl(uint32_t hClient, uint32_t hObject, uint32_t cmd, void *params, size_t paramsSize);
    bool rmFree(uint32_t hClient, uint32_t hParent, uint32_t hObject);

    // Helpers to generate unique handles
    uint32_t nextHandle() { return ++lastHandle; }

private:
    uint32_t lastHandle; // Simple handle generator

    // Hardware references
    IOPCIDevice *pciDevice;
    volatile uint32_t *mmioBase;

    // State
    bool initialized;
    bool gspReady;
    uint32_t rpcSeqNum;

    // DMA Buffers
    IOBufferMemoryDescriptor *cmdQueueMem;    // Command queue (host -> GSP)
    IOBufferMemoryDescriptor *statQueueMem;   // Status queue (GSP -> host)
    IOBufferMemoryDescriptor *firmwareMem;    // GSP firmware image
    IOBufferMemoryDescriptor *bootloaderMem;  // Bootloader ucode
    IOBufferMemoryDescriptor *wprMetaMem;     // WPR metadata
    IOBufferMemoryDescriptor *radix3Mem;      // Radix3 page table

    // Queue pointers
    volatile uint8_t *cmdQueue;
    volatile uint8_t *statQueue;
    uint32_t cmdQueueHead;
    uint32_t cmdQueueTail;
    uint32_t statQueueHead;
    uint32_t statQueueTail;

    // Physical addresses (for DMA)
    uint64_t cmdQueuePhys;
    uint64_t statQueuePhys;
    uint64_t firmwarePhys;
    uint64_t bootloaderPhys;
    uint64_t wprMetaPhys;
    uint64_t radix3Phys;

    // Firmware info
    uint64_t firmwareSize;
    uint64_t firmwareCodeOffset;
    uint64_t firmwareDataOffset;

    // Private methods
    uint32_t readReg(uint32_t offset);
    void writeReg(uint32_t offset, uint32_t value);

    bool allocDmaBuffer(IOBufferMemoryDescriptor **desc, size_t size, uint64_t *physAddr);
    void freeDmaBuffer(IOBufferMemoryDescriptor **desc);

    bool buildRadix3PageTable(const void *firmware, size_t size);
    bool setupWprMeta(void);

    bool resetFalcon(void);
    bool executeBootloader(void);
    bool executeFwsec(void);
    bool startRiscv(void);

    uint32_t calcChecksum(const void *data, size_t size);

    // Queue operations
    bool enqueueCommand(const void *msg, size_t size);
    bool dequeueStatus(void *msg, size_t maxSize, size_t *actualSize);
    void updateQueuePointers(void);

    // Constants
    static const size_t QUEUE_SIZE = 0x40000;         // 256KB per queue
    static const size_t GSP_HEAP_SIZE = 0x8100000;    // 129MB heap
    static const size_t FRTS_SIZE = 0x100000;         // 1MB FRTS
    static const size_t GSP_PAGE_SIZE = 4096;
};

// ============================================================================
// Inline implementations for performance-critical register access
// ============================================================================

inline uint32_t NVDAALGsp::readReg(uint32_t offset) {
    return mmioBase[offset / 4];
}

inline void NVDAALGsp::writeReg(uint32_t offset, uint32_t value) {
    mmioBase[offset / 4] = value;
    __sync_synchronize();
}

#endif // NVDAAL_GSP_H
