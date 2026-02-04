// Simple WPR2 Status Checker for NVIDIA GPUs on macOS
// Uses IOKit to find GPU info

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>

#define NVIDIA_VENDOR_ID 0x10DE

int main(int argc, char *argv[]) {
    io_iterator_t iterator;
    io_service_t service;
    kern_return_t kr;

    CFMutableDictionaryRef matchDict = IOServiceMatching("IOPCIDevice");
    kr = IOServiceGetMatchingServices(kIOMainPortDefault, matchDict, &iterator);

    if (kr != KERN_SUCCESS) {
        printf("Failed to get PCI services\n");
        return 1;
    }

    int found = 0;
    while ((service = IOIteratorNext(iterator)) != 0) {
        CFTypeRef vendorRef = IORegistryEntryCreateCFProperty(service, CFSTR("vendor-id"), kCFAllocatorDefault, 0);
        CFTypeRef deviceRef = IORegistryEntryCreateCFProperty(service, CFSTR("device-id"), kCFAllocatorDefault, 0);

        if (vendorRef && deviceRef) {
            uint32_t vendor = 0, device = 0;

            // vendor-id and device-id are CFData, not CFNumber
            if (CFGetTypeID(vendorRef) == CFDataGetTypeID()) {
                CFDataRef vendorData = (CFDataRef)vendorRef;
                if (CFDataGetLength(vendorData) >= 2) {
                    const uint8_t *bytes = CFDataGetBytePtr(vendorData);
                    vendor = bytes[0] | (bytes[1] << 8);
                }
            }

            if (CFGetTypeID(deviceRef) == CFDataGetTypeID()) {
                CFDataRef deviceData = (CFDataRef)deviceRef;
                if (CFDataGetLength(deviceData) >= 2) {
                    const uint8_t *bytes = CFDataGetBytePtr(deviceData);
                    device = bytes[0] | (bytes[1] << 8);
                }
            }

            if (vendor == NVIDIA_VENDOR_ID) {
                found = 1;
                printf("Found NVIDIA GPU: 0x%04X:0x%04X\n", vendor, device);

                // Get device name
                io_name_t name;
                IORegistryEntryGetName(service, name);
                printf("Device Name: %s\n", name);

                // Get class-code
                CFTypeRef classRef = IORegistryEntryCreateCFProperty(service, CFSTR("class-code"), kCFAllocatorDefault, 0);
                if (classRef && CFGetTypeID(classRef) == CFDataGetTypeID()) {
                    CFDataRef classData = (CFDataRef)classRef;
                    if (CFDataGetLength(classData) >= 3) {
                        const uint8_t *bytes = CFDataGetBytePtr(classData);
                        printf("Class Code: 0x%02X%02X%02X (VGA: %s)\n",
                               bytes[2], bytes[1], bytes[0],
                               (bytes[2] == 0x03) ? "YES" : "NO");
                    }
                    CFRelease(classRef);
                }

                printf("\n");
            }

            CFRelease(vendorRef);
            CFRelease(deviceRef);
        }

        IOObjectRelease(service);
    }

    IOObjectRelease(iterator);

    if (found) {
        printf("=== WPR2 Status ===\n");
        printf("Direct register access requires kernel-level access.\n");
        printf("The WPR2 status is shown by the EFI driver during boot.\n");
        printf("\n");
        printf("Did you see the NVDAAL v0.5 messages during boot?\n");
        printf("Please tell me what you saw for:\n");
        printf("  - METHOD 1: Power Cycle result\n");
        printf("  - METHOD 2: BROM Interface result\n");
        printf("  - METHOD 3: Direct Load result\n");
        printf("  - Final WPR2 Enabled: YES or NO?\n");
    } else {
        printf("No NVIDIA GPU found\n");
    }

    return 0;
}
