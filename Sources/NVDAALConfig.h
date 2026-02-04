/*
 * NVDAALConfig.h - Boot arguments and configuration system
 *
 * Inspired by Lilu's configuration system. Parses boot-args and provides
 * a centralized configuration interface.
 *
 * Boot arguments:
 *   -nvdaaloff       Disable NVDAAL completely
 *   -nvdaaldbg       Enable debug logging
 *   -nvdaalverbose   Enable verbose logging
 *   -nvdaalbeta      Allow loading on unsupported macOS versions
 *   -nvdaalforce     Force loading even in safe mode
 *   -nvdaalgsp=X     Override GSP firmware path
 *   nvdaal_loglevel=N  Set log level (0-5)
 */

#ifndef NVDAAL_CONFIG_H
#define NVDAAL_CONFIG_H

#include <libkern/libkern.h>
#include <IOKit/IOLib.h>
#include "NVDAALDebug.h"

// =============================================================================
// Boot Argument Names
// =============================================================================

#define NVDAAL_BOOTARG_OFF       "-nvdaaloff"
#define NVDAAL_BOOTARG_DEBUG     "-nvdaaldbg"
#define NVDAAL_BOOTARG_VERBOSE   "-nvdaalverbose"
#define NVDAAL_BOOTARG_BETA      "-nvdaalbeta"
#define NVDAAL_BOOTARG_FORCE     "-nvdaalforce"
#define NVDAAL_BOOTARG_LOGLEVEL  "nvdaal_loglevel"
#define NVDAAL_BOOTARG_GSPPATH   "nvdaal_gsp"

// =============================================================================
// Configuration State
// =============================================================================

struct NVDAALConfiguration {
    // Parsed from boot-args
    bool disabled;           // -nvdaaloff
    bool debugEnabled;       // -nvdaaldbg
    bool verboseEnabled;     // -nvdaalverbose
    bool betaAllowed;        // -nvdaalbeta
    bool forceLoad;          // -nvdaalforce
    int  logLevel;           // nvdaal_loglevel=N

    // Runtime state
    bool safeMode;           // Booted in safe mode
    bool recoveryMode;       // Booted in recovery
    bool installerMode;      // Booted in installer

    // Kernel version
    int kernelMajor;         // e.g., 26 for macOS 26
    int kernelMinor;

    // Firmware paths (if overridden)
    char gspFirmwarePath[256];
};

extern NVDAALConfiguration nvdaalConfig;

// =============================================================================
// Configuration API
// =============================================================================

/**
 * Parse boot arguments and initialize configuration.
 * Call this early in kext start.
 */
static inline void nvdaalConfigInit(void) {
    // Clear config
    bzero(&nvdaalConfig, sizeof(nvdaalConfig));
    nvdaalConfig.logLevel = NVDAAL_LOG_INFO;

    // Parse boot-args
    nvdaalConfig.disabled = PE_parse_boot_argn(NVDAAL_BOOTARG_OFF, nullptr, 0);
    nvdaalConfig.debugEnabled = PE_parse_boot_argn(NVDAAL_BOOTARG_DEBUG, nullptr, 0);
    nvdaalConfig.verboseEnabled = PE_parse_boot_argn(NVDAAL_BOOTARG_VERBOSE, nullptr, 0);
    nvdaalConfig.betaAllowed = PE_parse_boot_argn(NVDAAL_BOOTARG_BETA, nullptr, 0);
    nvdaalConfig.forceLoad = PE_parse_boot_argn(NVDAAL_BOOTARG_FORCE, nullptr, 0);

    // Parse log level
    int level = 0;
    if (PE_parse_boot_argn(NVDAAL_BOOTARG_LOGLEVEL, &level, sizeof(level))) {
        if (level >= NVDAAL_LOG_NONE && level <= NVDAAL_LOG_VERBOSE) {
            nvdaalConfig.logLevel = level;
        }
    }

    // Override log level based on flags
    if (nvdaalConfig.verboseEnabled) {
        nvdaalConfig.logLevel = NVDAAL_LOG_VERBOSE;
    } else if (nvdaalConfig.debugEnabled) {
        nvdaalConfig.logLevel = NVDAAL_LOG_DEBUG;
    }

    // Parse GSP firmware path
    PE_parse_boot_argn(NVDAAL_BOOTARG_GSPPATH,
                       nvdaalConfig.gspFirmwarePath,
                       sizeof(nvdaalConfig.gspFirmwarePath));

    // Detect boot mode
    int safeMode = 0;
    if (PE_parse_boot_argn("-x", &safeMode, sizeof(safeMode))) {
        nvdaalConfig.safeMode = true;
    }

    // Get kernel version
    char osversion[64] = {0};
    if (PE_parse_boot_argn("osversion", osversion, sizeof(osversion))) {
        // Parse version string (e.g., "26.0.0")
        sscanf(osversion, "%d.%d", &nvdaalConfig.kernelMajor, &nvdaalConfig.kernelMinor);
    }

    // Set global debug state
    nvdaalLogLevel = (NVDAALLogLevel)nvdaalConfig.logLevel;
    nvdaalDebugEnabled = nvdaalConfig.debugEnabled;
}

/**
 * Check if NVDAAL should load based on configuration.
 * Returns true if loading should proceed.
 */
static inline bool nvdaalShouldLoad(void) {
    // Check if explicitly disabled
    if (nvdaalConfig.disabled) {
        IOLog("NVDAAL: Disabled via boot-arg\n");
        return false;
    }

    // Check safe mode
    if (nvdaalConfig.safeMode && !nvdaalConfig.forceLoad) {
        IOLog("NVDAAL: Refusing to load in safe mode (use -nvdaalforce)\n");
        return false;
    }

    // Check macOS version compatibility
    // Supported: macOS 26+ (Tahoe)
    const int MIN_KERNEL_MAJOR = 26;
    if (nvdaalConfig.kernelMajor < MIN_KERNEL_MAJOR && !nvdaalConfig.betaAllowed) {
        IOLog("NVDAAL: Unsupported macOS version %d.%d (use -nvdaalbeta)\n",
              nvdaalConfig.kernelMajor, nvdaalConfig.kernelMinor);
        return false;
    }

    return true;
}

/**
 * Log current configuration (debug).
 */
static inline void nvdaalConfigLog(void) {
    NVDLOG("config", "Configuration:");
    NVDLOG("config", "  disabled=%d debug=%d verbose=%d beta=%d force=%d",
           nvdaalConfig.disabled, nvdaalConfig.debugEnabled,
           nvdaalConfig.verboseEnabled, nvdaalConfig.betaAllowed,
           nvdaalConfig.forceLoad);
    NVDLOG("config", "  logLevel=%d safeMode=%d",
           nvdaalConfig.logLevel, nvdaalConfig.safeMode);
    NVDLOG("config", "  macOS=%d.%d",
           nvdaalConfig.kernelMajor, nvdaalConfig.kernelMinor);
    if (nvdaalConfig.gspFirmwarePath[0]) {
        NVDLOG("config", "  gspPath=%s", nvdaalConfig.gspFirmwarePath);
    }
}

// =============================================================================
// Convenience Macros
// =============================================================================

// Check if debug output is enabled
#define NVDAAL_DEBUG_ENABLED   (nvdaalConfig.debugEnabled)
#define NVDAAL_VERBOSE_ENABLED (nvdaalConfig.verboseEnabled)

// Feature flags (for future use)
#define NVDAAL_FEATURE_ENABLED(name) (nvdaalConfig.features & NVDAAL_FEATURE_##name)

#endif // NVDAAL_CONFIG_H
