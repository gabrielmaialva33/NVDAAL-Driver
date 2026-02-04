/*
 * NVDAALDebug.h - Debug logging system inspired by Lilu
 *
 * Provides structured logging with levels, similar to Lilu's DBGLOG/SYSLOG.
 * Usage:
 *   NVDDBG("component", "message %d", value);  // Debug (only in DEBUG builds)
 *   NVDLOG("component", "message %d", value);  // Always logs
 *   NVDERR("component", "message %d", value);  // Error level
 */

#ifndef NVDAAL_DEBUG_H
#define NVDAAL_DEBUG_H

#include <libkern/libkern.h>

// =============================================================================
// Log Levels
// =============================================================================

enum NVDAALLogLevel {
    NVDAAL_LOG_NONE    = 0,
    NVDAAL_LOG_ERROR   = 1,
    NVDAAL_LOG_WARN    = 2,
    NVDAAL_LOG_INFO    = 3,
    NVDAAL_LOG_DEBUG   = 4,
    NVDAAL_LOG_VERBOSE = 5
};

// =============================================================================
// Global Debug State
// =============================================================================

extern NVDAALLogLevel nvdaalLogLevel;
extern bool nvdaalDebugEnabled;

// =============================================================================
// Core Logging Macros
// =============================================================================

// Internal log function (always available)
#define NVDAAL_LOG_IMPL(level, prefix, component, fmt, ...) do { \
    if (nvdaalLogLevel >= (level)) { \
        IOLog("NVDAAL" prefix "[%s] " fmt "\n", component, ##__VA_ARGS__); \
    } \
} while(0)

// Error logging (always enabled)
#define NVDERR(component, fmt, ...) \
    NVDAAL_LOG_IMPL(NVDAAL_LOG_ERROR, "-ERR", component, fmt, ##__VA_ARGS__)

// Warning logging
#define NVDWARN(component, fmt, ...) \
    NVDAAL_LOG_IMPL(NVDAAL_LOG_WARN, "-WARN", component, fmt, ##__VA_ARGS__)

// Info logging (default)
#define NVDLOG(component, fmt, ...) \
    NVDAAL_LOG_IMPL(NVDAAL_LOG_INFO, "", component, fmt, ##__VA_ARGS__)

// Debug logging (DEBUG builds or -nvdaaldbg boot-arg)
#ifdef DEBUG
#define NVDDBG(component, fmt, ...) \
    NVDAAL_LOG_IMPL(NVDAAL_LOG_DEBUG, "-DBG", component, fmt, ##__VA_ARGS__)
#else
#define NVDDBG(component, fmt, ...) do { \
    if (nvdaalDebugEnabled) { \
        NVDAAL_LOG_IMPL(NVDAAL_LOG_DEBUG, "-DBG", component, fmt, ##__VA_ARGS__); \
    } \
} while(0)
#endif

// Verbose logging (only with -nvdaalverbose)
#define NVDVERBOSE(component, fmt, ...) \
    NVDAAL_LOG_IMPL(NVDAAL_LOG_VERBOSE, "-V", component, fmt, ##__VA_ARGS__)

// =============================================================================
// Panic Helpers (like Lilu's PANIC)
// =============================================================================

#define NVDPANIC(component, fmt, ...) do { \
    IOLog("NVDAAL-PANIC[%s] " fmt "\n", component, ##__VA_ARGS__); \
    panic("NVDAAL[%s]: " fmt, component, ##__VA_ARGS__); \
} while(0)

// Conditional panic (debug builds only)
#ifdef DEBUG
#define NVDPANIC_DBG(component, fmt, ...) NVDPANIC(component, fmt, ##__VA_ARGS__)
#else
#define NVDPANIC_DBG(component, fmt, ...) NVDERR(component, fmt, ##__VA_ARGS__)
#endif

// =============================================================================
// Assertion Macros
// =============================================================================

#define NVDASSERT(expr, component, fmt, ...) do { \
    if (!(expr)) { \
        NVDERR(component, "ASSERT FAILED: " #expr " - " fmt, ##__VA_ARGS__); \
    } \
} while(0)

#ifdef DEBUG
#define NVDASSERT_DBG(expr, component, fmt, ...) NVDASSERT(expr, component, fmt, ##__VA_ARGS__)
#else
#define NVDASSERT_DBG(expr, component, fmt, ...) ((void)0)
#endif

// =============================================================================
// Hex Dump Helper
// =============================================================================

static inline void nvdaalHexDump(const char *component, const void *data, size_t size, size_t maxLines = 8) {
    if (nvdaalLogLevel < NVDAAL_LOG_DEBUG) return;

    const uint8_t *bytes = (const uint8_t *)data;
    size_t lines = (size + 15) / 16;
    if (lines > maxLines) lines = maxLines;

    for (size_t i = 0; i < lines; i++) {
        char line[80];
        char *p = line;
        p += snprintf(p, sizeof(line), "%04zx: ", i * 16);

        for (size_t j = 0; j < 16 && (i * 16 + j) < size; j++) {
            p += snprintf(p, (size_t)(line + sizeof(line) - p), "%02x ", bytes[i * 16 + j]);
        }

        IOLog("NVDAAL-DBG[%s] %s\n", component, line);
    }

    if ((size + 15) / 16 > maxLines) {
        IOLog("NVDAAL-DBG[%s] ... (%zu more bytes)\n", component, size - maxLines * 16);
    }
}

#define NVDHEXDUMP(component, data, size) nvdaalHexDump(component, data, size)

// =============================================================================
// Timing Helpers
// =============================================================================

#define NVDTIMED_START(name) uint64_t _nvd_time_##name = mach_absolute_time()

#define NVDTIMED_END(component, name, fmt, ...) do { \
    uint64_t _nvd_elapsed = mach_absolute_time() - _nvd_time_##name; \
    NVDDBG(component, fmt " took %llu ticks", ##__VA_ARGS__, _nvd_elapsed); \
} while(0)

#endif // NVDAAL_DEBUG_H
