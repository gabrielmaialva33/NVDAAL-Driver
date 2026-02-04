/*
 * NVDAALVersion.h - Version information and compatibility
 *
 * Inspired by Lilu's version system. Provides version constants,
 * compatibility checks, and kernel version detection.
 */

#ifndef NVDAAL_VERSION_H
#define NVDAAL_VERSION_H

#include <stdint.h>

// =============================================================================
// NVDAAL Version
// =============================================================================

#define NVDAAL_VERSION_MAJOR   0
#define NVDAAL_VERSION_MINOR   7
#define NVDAAL_VERSION_PATCH   0

// Combined version for comparison (0x00MMNNPP)
#define NVDAAL_VERSION \
    ((NVDAAL_VERSION_MAJOR << 16) | (NVDAAL_VERSION_MINOR << 8) | NVDAAL_VERSION_PATCH)

// Version string
#define NVDAAL_VERSION_STR     "0.7.0"

// Build info
#ifdef DEBUG
#define NVDAAL_BUILD_TYPE      "DEBUG"
#else
#define NVDAAL_BUILD_TYPE      "RELEASE"
#endif

// =============================================================================
// Supported macOS Versions
// =============================================================================

// Minimum supported macOS (kernel major version)
#define NVDAAL_MIN_KERNEL_MAJOR    26   // macOS 26 (Tahoe)

// Maximum tested macOS
#define NVDAAL_MAX_KERNEL_MAJOR    26   // macOS 26 (Tahoe)

// =============================================================================
// Vendor ID
// =============================================================================

#define NVIDIA_VENDOR_ID    0x10DE

// =============================================================================
// Supported GPU Device IDs
// =============================================================================

#define NVDAAL_SUPPORTED_DEVICES \
    X(0x2684, "RTX 4090")         \
    X(0x2685, "RTX 4090 D")       \
    X(0x2702, "RTX 4080 Super")   \
    X(0x2704, "RTX 4080")         \
    X(0x2705, "RTX 4070 Ti Super")\
    X(0x2782, "RTX 4070 Ti")      \
    X(0x2786, "RTX 4070")         \
    X(0x2860, "RTX 4070 Super")

// Check if device ID is supported
static inline bool nvdaalIsDeviceSupported(uint16_t deviceId) {
    switch (deviceId) {
        #define X(id, name) case id:
        NVDAAL_SUPPORTED_DEVICES
        #undef X
            return true;
        default:
            return false;
    }
}

// Get device name from ID
static inline const char *nvdaalGetDeviceName(uint16_t deviceId) {
    switch (deviceId) {
        #define X(id, name) case id: return name;
        NVDAAL_SUPPORTED_DEVICES
        #undef X
        default:
            return "Unknown";
    }
}

// =============================================================================
// Architecture Detection
// =============================================================================

// NVIDIA GPU architectures (from NV_PMC_BOOT_0)
#define NVDAAL_ARCH_AMPERE     0x17   // GA1xx (RTX 30 series)
#define NVDAAL_ARCH_ADA        0x19   // AD1xx (RTX 40 series)
#define NVDAAL_ARCH_BLACKWELL  0x1B   // GB2xx (RTX 50 series)

static inline const char *nvdaalGetArchName(uint8_t arch) {
    switch (arch) {
        case NVDAAL_ARCH_AMPERE:    return "Ampere";
        case NVDAAL_ARCH_ADA:       return "Ada Lovelace";
        case NVDAAL_ARCH_BLACKWELL: return "Blackwell";
        default:                    return "Unknown";
    }
}

static inline bool nvdaalIsArchSupported(uint8_t arch) {
    // Currently only Ada Lovelace is supported
    return arch == NVDAAL_ARCH_ADA;
}

// =============================================================================
// Feature Flags
// =============================================================================

// Runtime feature detection
typedef enum {
    NVDAAL_FEATURE_GSP          = (1 << 0),  // GSP required/available
    NVDAAL_FEATURE_WPR2         = (1 << 1),  // WPR2 support
    NVDAAL_FEATURE_COMPUTE      = (1 << 2),  // Compute queues
    NVDAAL_FEATURE_DISPLAY      = (1 << 3),  // Display (disabled)
    NVDAAL_FEATURE_NVENC        = (1 << 4),  // NVENC
    NVDAAL_FEATURE_NVDEC        = (1 << 5),  // NVDEC
} NVDAALFeatureFlags;

// Default features for Ada
#define NVDAAL_ADA_FEATURES \
    (NVDAAL_FEATURE_GSP | NVDAAL_FEATURE_WPR2 | NVDAAL_FEATURE_COMPUTE)

// =============================================================================
// Plugin API Version
// =============================================================================

// For future plugin support (like Lilu plugins)
#define NVDAAL_PLUGIN_API_VERSION  1

// Plugin compatibility check
#define NVDAAL_PLUGIN_CHECK(pluginApiVersion) \
    ((pluginApiVersion) == NVDAAL_PLUGIN_API_VERSION)

// =============================================================================
// Build Information
// =============================================================================

// Compile-time info
#define NVDAAL_BUILD_DATE      __DATE__
#define NVDAAL_BUILD_TIME      __TIME__

#if defined(__clang__)
#define NVDAAL_COMPILER        "Clang " __clang_version__
#elif defined(__GNUC__)
#define NVDAAL_COMPILER        "GCC " __VERSION__
#else
#define NVDAAL_COMPILER        "Unknown"
#endif

// =============================================================================
// Banner
// =============================================================================

#define NVDAAL_BANNER \
    "NVDAAL v" NVDAAL_VERSION_STR " (" NVDAAL_BUILD_TYPE ") - NVIDIA Ada Lovelace Compute Driver\n" \
    "Built: " NVDAAL_BUILD_DATE " " NVDAAL_BUILD_TIME " with " NVDAAL_COMPILER "\n"

#endif // NVDAAL_VERSION_H
