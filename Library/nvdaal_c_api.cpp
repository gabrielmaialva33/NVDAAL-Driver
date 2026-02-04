/*
 * nvdaal_c_api.cpp - C-compatible wrapper for libNVDAAL
 *
 * This allows Python (via ctypes) or other languages to use the driver.
 * All functions handle NULL client gracefully.
 */

#include "libNVDAAL.h"
#include <new>

extern "C" {

void* nvdaal_create_client() {
    return new (std::nothrow) nvdaal::Client();
}

void nvdaal_destroy_client(void* client) {
    if (client) {
        delete static_cast<nvdaal::Client*>(client);
    }
}

bool nvdaal_connect(void* client) {
    if (!client) return false;
    return static_cast<nvdaal::Client*>(client)->connect();
}

void nvdaal_disconnect(void* client) {
    if (client) {
        static_cast<nvdaal::Client*>(client)->disconnect();
    }
}

bool nvdaal_is_connected(void* client) {
    if (!client) return false;
    return static_cast<nvdaal::Client*>(client)->isConnected();
}

uint64_t nvdaal_alloc_vram(void* client, size_t size) {
    if (!client || size == 0) return 0;
    return static_cast<nvdaal::Client*>(client)->allocVram(size);
}

bool nvdaal_submit_command(void* client, uint32_t cmd) {
    if (!client) return false;
    return static_cast<nvdaal::Client*>(client)->submitCommand(cmd);
}

bool nvdaal_load_firmware(void* client, const char* path) {
    if (!client || !path) return false;
    return static_cast<nvdaal::Client*>(client)->loadFirmware(path);
}

bool nvdaal_execute_fwsec(void* client) {
    if (!client) return false;
    return static_cast<nvdaal::Client*>(client)->executeFwsec();
}

bool nvdaal_get_status(void* client, uint32_t* pmc_boot0, uint32_t* wpr2_lo,
                       uint32_t* wpr2_hi, bool* wpr2_enabled) {
    if (!client) return false;
    nvdaal::GpuStatus status;
    bool ok = static_cast<nvdaal::Client*>(client)->getStatus(&status);
    if (ok) {
        if (pmc_boot0) *pmc_boot0 = status.pmcBoot0;
        if (wpr2_lo) *wpr2_lo = status.wpr2Lo;
        if (wpr2_hi) *wpr2_hi = status.wpr2Hi;
        if (wpr2_enabled) *wpr2_enabled = status.wpr2Enabled;
    }
    return ok;
}

}
