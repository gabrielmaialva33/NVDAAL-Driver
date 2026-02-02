/*
 * nvdaal-cli - User-space tool for NVDAAL Driver
 *
 * Usage: nvdaal-cli boot <firmware_dir>
 *        nvdaal-cli load <gsp.bin>
 */

#include <iostream>
#include <string>
#include <fstream>
#include "../../Library/libNVDAAL.h"

void print_usage(const char *prog) {
    std::cout << "Usage: " << prog << " <command> [args]\n";
    std::cout << "Commands:\n";
    std::cout << "  boot <firmware_dir>   Full boot sequence with all firmwares\n";
    std::cout << "  load <firmware.bin>   Load GSP firmware only (legacy)\n";
    std::cout << "  test                  Verify VRAM allocation and Queue kicking\n";
    std::cout << "\n";
    std::cout << "Full Boot Sequence (boot command):\n";
    std::cout << "  Expects these files in <firmware_dir>:\n";
    std::cout << "    - gsp-570.144.bin (or gsp.bin) - GSP-RM firmware\n";
    std::cout << "    - booter_load-ad102-570.144.bin (optional) - SEC2 booter\n";
    std::cout << "    - AD102.rom (optional) - VBIOS for FWSEC\n";
}

bool file_exists(const std::string& path) {
    std::ifstream f(path);
    return f.good();
}

int cmd_boot(const std::string& firmwareDir) {
    nvdaal::Client client;

    if (!client.connect()) {
        std::cerr << "[-] Error: Could not connect to driver." << std::endl;
        return 1;
    }

    std::cout << "[*] NVDAAL Boot Sequence" << std::endl;
    std::cout << "[*] Firmware directory: " << firmwareDir << std::endl;

    // Paths to firmware files
    std::string gspPath = firmwareDir + "/gsp-570.144.bin";
    std::string booterPath = firmwareDir + "/booter_load-ad102-570.144.bin";
    std::string vbiosPath = firmwareDir + "/AD102.rom";

    // Fall back to generic names
    if (!file_exists(gspPath)) {
        gspPath = firmwareDir + "/gsp.bin";
    }

    // Check required GSP firmware
    if (!file_exists(gspPath)) {
        std::cerr << "[-] Error: GSP firmware not found" << std::endl;
        std::cerr << "    Expected: " << firmwareDir << "/gsp-570.144.bin or gsp.bin" << std::endl;
        return 1;
    }

    // Step 1: Load VBIOS (optional - for FWSEC)
    if (file_exists(vbiosPath)) {
        std::cout << "[1] Loading VBIOS for FWSEC..." << std::endl;
        if (client.loadVbios(vbiosPath)) {
            std::cout << "    OK: VBIOS loaded" << std::endl;
        } else {
            std::cerr << "    Warning: VBIOS load failed (continuing without FWSEC)" << std::endl;
        }
    } else {
        std::cout << "[1] VBIOS not found, skipping FWSEC (may be done by EFI)" << std::endl;
    }

    // Step 2: Load booter_load (optional - for SEC2)
    if (file_exists(booterPath)) {
        std::cout << "[2] Loading booter_load for SEC2..." << std::endl;
        if (client.loadBooterLoad(booterPath)) {
            std::cout << "    OK: booter_load loaded" << std::endl;
        } else {
            std::cerr << "    Warning: booter_load failed (continuing with direct boot)" << std::endl;
        }
    } else {
        std::cout << "[2] booter_load not found, using direct boot" << std::endl;
    }

    // Step 3: Load GSP firmware (required)
    std::cout << "[3] Loading GSP firmware: " << gspPath << std::endl;
    if (client.loadFirmware(gspPath)) {
        std::cout << "[+] SUCCESS: GSP firmware loaded and boot sequence initiated!" << std::endl;
        return 0;
    } else {
        std::cerr << "[-] ERROR: GSP firmware load failed. Check kernel logs." << std::endl;
        std::cerr << "    Use: log show --predicate 'eventMessage contains \"NVDAAL\"' --last 1m" << std::endl;
        return 1;
    }
}

int cmd_load_firmware(const std::string& path) {
    nvdaal::Client client;

    std::cout << "[*] Loading firmware from " << path << "..." << std::endl;

    if (client.loadFirmware(path)) {
        std::cout << "[+] SUCCESS: Firmware uploaded and GSP boot initiated!" << std::endl;
        return 0;
    } else {
        std::cerr << "[-] ERROR: Failed to load firmware. Check logs." << std::endl;
        return 1;
    }
}

int cmd_test() {
    nvdaal::Client client;

    if (!client.connect()) {
        std::cerr << "[-] Error: Could not connect to driver." << std::endl;
        return 1;
    }

    std::cout << "[*] Testing VRAM allocation (1MB)..." << std::endl;
    uint64_t vramOffset = client.allocVram(1024 * 1024);

    if (vramOffset != 0) {
        std::cout << "[+] SUCCESS: Allocated at 0x" << std::hex << vramOffset << std::endl;
    } else {
        std::cerr << "[-] FAILED: VRAM allocation returned 0" << std::endl;
    }

    std::cout << "[*] Testing Compute Queue kick..." << std::endl;
    if (client.submitCommand(0xCAFEBABE)) {
        std::cout << "[+] SUCCESS: Command submitted!" << std::endl;
    } else {
        std::cerr << "[-] FAILED: Command submission error" << std::endl;
    }

    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::string command = argv[1];

    if (command == "boot") {
        if (argc < 3) {
            std::cerr << "Error: Missing firmware directory path" << std::endl;
            print_usage(argv[0]);
            return 1;
        }
        return cmd_boot(argv[2]);
    } else if (command == "load") {
        if (argc < 3) {
            std::cerr << "Error: Missing firmware path" << std::endl;
            print_usage(argv[0]);
            return 1;
        }
        return cmd_load_firmware(argv[2]);
    } else if (command == "test") {
        return cmd_test();
    } else {
        std::cerr << "Unknown command: " << command << std::endl;
        print_usage(argv[0]);
        return 1;
    }
}
