/*
 * libNVDAAL.cpp - Implementation of User-Space SDK
 */

#include "libNVDAAL.h"
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <fstream>
#include <iostream>

// Matches UserClient selectors
#define SERVICE_NAME "NVDAAL"
#define METHOD_LOAD_FIRMWARE 0
#define METHOD_ALLOC_VRAM 1
#define METHOD_SUBMIT_CMD 2
#define METHOD_WAIT_SYNC 3
#define METHOD_LOAD_BOOTER 4
#define METHOD_LOAD_VBIOS 5
#define METHOD_LOAD_BOOTLOADER 6
#define METHOD_GET_STATUS 7
#define METHOD_EXECUTE_FWSEC 8

namespace nvdaal {

Client::Client() : connection(0), connected(false) {}

Client::~Client() {
    disconnect();
}

bool Client::connect() {
    if (connected) return true;

    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceNameMatching(SERVICE_NAME));
    if (!service) {
        std::cerr << "[libNVDAAL] Error: NVDAAL driver service not found." << std::endl;
        return false;
    }

    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, (io_connect_t*)&connection);
    IOObjectRelease(service);

    if (kr != KERN_SUCCESS) {
        std::cerr << "[libNVDAAL] Error: IOServiceOpen failed (0x" << std::hex << kr << ")" << std::endl;
        return false;
    }

    connected = true;
    return true;
}

void Client::disconnect() {
    if (connected && connection) {
        IOServiceClose((io_connect_t)connection);
        connection = 0;
        connected = false;
    }
}

bool Client::isConnected() const {
    return connected;
}

bool Client::loadFirmware(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "[libNVDAAL] Error: Could not open file " << path << std::endl;
        return false;
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    if (size <= 0) return false;

    std::vector<uint8_t> buffer(size);
    if (!file.read((char*)buffer.data(), size)) return false;

    return loadFirmware(buffer.data(), size);
}

bool Client::loadFirmware(const void* data, size_t size) {
    if (!connect()) return false;

    uint64_t args[2] = { (uint64_t)data, (uint64_t)size };

    kern_return_t kr = IOConnectCallScalarMethod(
        (io_connect_t)connection,
        METHOD_LOAD_FIRMWARE,
        args, 2,
        NULL, NULL
    );

    if (kr != KERN_SUCCESS) {
        std::cerr << "[libNVDAAL] loadFirmware failed: 0x" << std::hex << kr << std::dec << std::endl;
    }

    return (kr == KERN_SUCCESS);
}

uint64_t Client::allocVram(size_t size) {
    if (!connect()) return 0;

    uint64_t input[1] = { (uint64_t)size };
    uint64_t output[1] = { 0 };
    uint32_t outputCount = 1;

    kern_return_t kr = IOConnectCallScalarMethod(
        (io_connect_t)connection,
        METHOD_ALLOC_VRAM,
        input, 1,
        output, &outputCount
    );

    if (kr != KERN_SUCCESS) return 0;
    return output[0];
}

bool Client::submitCommand(uint32_t cmd) {
    if (!connect()) return false;

    uint64_t input[1] = { (uint64_t)cmd };

    kern_return_t kr = IOConnectCallScalarMethod(
        (io_connect_t)connection,
        METHOD_SUBMIT_CMD,
        input, 1,
        NULL, NULL
    );

    return (kr == KERN_SUCCESS);
}

bool Client::waitSemaphore(uint64_t gpuAddr, uint32_t value) {
    if (!connect()) return false;

    uint64_t input[2] = { gpuAddr, (uint64_t)value };

    kern_return_t kr = IOConnectCallScalarMethod(
        (io_connect_t)connection,
        METHOD_WAIT_SYNC,
        input, 2,
        NULL, NULL
    );

    return (kr == KERN_SUCCESS);
}

bool Client::loadBootloader(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "[libNVDAAL] Error: Could not open file " << path << std::endl;
        return false;
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    if (size <= 0) return false;

    std::vector<uint8_t> buffer(size);
    if (!file.read((char*)buffer.data(), size)) return false;

    return loadBootloader(buffer.data(), size);
}

bool Client::loadBootloader(const void* data, size_t size) {
    if (!connect()) return false;

    uint64_t args[2] = { (uint64_t)data, (uint64_t)size };

    kern_return_t kr = IOConnectCallScalarMethod(
        (io_connect_t)connection,
        METHOD_LOAD_BOOTLOADER,
        args, 2,
        NULL, NULL
    );

    if (kr != KERN_SUCCESS) {
        std::cerr << "[libNVDAAL] loadBootloader failed: 0x" << std::hex << kr << std::dec << std::endl;
    }

    return (kr == KERN_SUCCESS);
}

bool Client::loadBooterLoad(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "[libNVDAAL] Error: Could not open file " << path << std::endl;
        return false;
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    if (size <= 0) return false;

    std::vector<uint8_t> buffer(size);
    if (!file.read((char*)buffer.data(), size)) return false;

    return loadBooterLoad(buffer.data(), size);
}

bool Client::loadBooterLoad(const void* data, size_t size) {
    if (!connect()) return false;

    uint64_t args[2] = { (uint64_t)data, (uint64_t)size };

    kern_return_t kr = IOConnectCallScalarMethod(
        (io_connect_t)connection,
        METHOD_LOAD_BOOTER,
        args, 2,
        NULL, NULL
    );

    if (kr != KERN_SUCCESS) {
        std::cerr << "[libNVDAAL] loadBooterLoad failed: 0x" << std::hex << kr << std::dec << std::endl;
    }

    return (kr == KERN_SUCCESS);
}

bool Client::loadVbios(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "[libNVDAAL] Error: Could not open file " << path << std::endl;
        return false;
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    if (size <= 0) return false;

    std::vector<uint8_t> buffer(size);
    if (!file.read((char*)buffer.data(), size)) return false;

    return loadVbios(buffer.data(), size);
}

bool Client::loadVbios(const void* data, size_t size) {
    if (!connect()) return false;

    uint64_t args[2] = { (uint64_t)data, (uint64_t)size };

    kern_return_t kr = IOConnectCallScalarMethod(
        (io_connect_t)connection,
        METHOD_LOAD_VBIOS,
        args, 2,
        NULL, NULL
    );

    if (kr != KERN_SUCCESS) {
        std::cerr << "[libNVDAAL] loadVbios failed: 0x" << std::hex << kr << std::dec << std::endl;
    }

    return (kr == KERN_SUCCESS);
}

bool Client::executeFwsec() {
    if (!connect()) return false;

    uint64_t output[1] = {0};
    uint32_t outputCount = 1;

    kern_return_t kr = IOConnectCallScalarMethod(
        (io_connect_t)connection,
        METHOD_EXECUTE_FWSEC,
        NULL, 0,           // No input
        output, &outputCount
    );

    if (kr != KERN_SUCCESS) {
        std::cerr << "[libNVDAAL] executeFwsec failed: 0x" << std::hex << kr << std::dec << std::endl;
        return false;
    }

    return (output[0] == 1);
}

bool Client::getStatus(GpuStatus *status) {
    if (!connect() || !status) return false;

    uint64_t output[9] = {0};
    uint32_t outputCount = 9;

    kern_return_t kr = IOConnectCallScalarMethod(
        (io_connect_t)connection,
        METHOD_GET_STATUS,
        NULL, 0,           // No input
        output, &outputCount
    );

    if (kr != KERN_SUCCESS) {
        std::cerr << "[libNVDAAL] getStatus failed: 0x" << std::hex << kr << std::dec << std::endl;
        return false;
    }

    status->pmcBoot0 = (uint32_t)output[0];
    status->wpr2Lo = (uint32_t)output[1];
    status->wpr2Hi = (uint32_t)output[2];
    status->wpr2Enabled = (output[3] != 0);
    status->gspRiscvCpuctl = (uint32_t)output[4];
    status->sec2RiscvCpuctl = (uint32_t)output[5];
    status->gspFalconMailbox0 = (uint32_t)output[6];
    status->gspFalconMailbox1 = (uint32_t)output[7];
    status->bootScratch = (uint32_t)output[8];

    return true;
}

} // namespace nvdaal
