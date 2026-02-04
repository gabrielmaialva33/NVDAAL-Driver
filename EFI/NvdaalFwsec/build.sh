#!/bin/bash
#
# Build script for NvdaalFwsec EFI driver
#
# Requirements:
# - EDK2 (https://github.com/tianocore/edk2)
# - Set EDK_TOOLS_PATH and WORKSPACE environment variables
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DRIVER_NAME="NvdaalFwsec"

echo "========================================="
echo "  Building $DRIVER_NAME EFI Driver"
echo "========================================="

# Check if EDK2 is set up
if [ -z "$WORKSPACE" ]; then
    echo "ERROR: WORKSPACE not set. Please source edksetup.sh first."
    echo ""
    echo "Quick setup:"
    echo "  cd /path/to/edk2"
    echo "  source edksetup.sh"
    echo "  export PACKAGES_PATH=\$WORKSPACE"
    exit 1
fi

# Create package directory if needed
PACKAGE_DIR="$WORKSPACE/NvdaalPkg"
if [ ! -d "$PACKAGE_DIR" ]; then
    echo "Creating NvdaalPkg in $WORKSPACE..."
    mkdir -p "$PACKAGE_DIR"
fi

# Copy source files
echo "Copying source files..."
cp -r "$SCRIPT_DIR"/* "$PACKAGE_DIR/"

# Create package DSC file
cat > "$PACKAGE_DIR/NvdaalPkg.dsc" << 'EOF'
[Defines]
  PLATFORM_NAME                  = NvdaalPkg
  PLATFORM_GUID                  = 12345678-1234-1234-1234-123456789ABC
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/NvdaalPkg
  SUPPORTED_ARCHITECTURES        = X64
  BUILD_TARGETS                  = DEBUG|RELEASE
  SKUID_IDENTIFIER               = DEFAULT

[LibraryClasses]
  UefiDriverEntryPoint|MdePkg/Library/UefiDriverEntryPoint/UefiDriverEntryPoint.inf
  UefiBootServicesTableLib|MdePkg/Library/UefiBootServicesTableLib/UefiBootServicesTableLib.inf
  UefiLib|MdePkg/Library/UefiLib/UefiLib.inf
  BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
  BaseMemoryLib|MdePkg/Library/BaseMemoryLib/BaseMemoryLib.inf
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf
  DebugLib|MdePkg/Library/BaseDebugLibNull/BaseDebugLibNull.inf
  PrintLib|MdePkg/Library/BasePrintLib/BasePrintLib.inf
  PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf
  UefiRuntimeServicesTableLib|MdePkg/Library/UefiRuntimeServicesTableLib/UefiRuntimeServicesTableLib.inf
  IoLib|MdePkg/Library/BaseIoLibIntrinsic/BaseIoLibIntrinsic.inf
  RegisterFilterLib|MdePkg/Library/RegisterFilterLibNull/RegisterFilterLibNull.inf
  StackCheckLib|MdePkg/Library/StackCheckLibNull/StackCheckLibNull.inf

[Components]
  NvdaalPkg/NvdaalFwsec.inf
EOF

# Build the driver
echo "Building driver..."
cd "$WORKSPACE"
build -p NvdaalPkg/NvdaalPkg.dsc -a X64 -t XCODE5 -b RELEASE

# Find the built driver
DRIVER_PATH=$(find "$WORKSPACE/Build/NvdaalPkg" -name "NvdaalFwsec.efi" | head -1)

if [ -n "$DRIVER_PATH" ]; then
    echo ""
    echo "========================================="
    echo "  BUILD SUCCESSFUL!"
    echo "========================================="
    echo ""
    echo "Driver location: $DRIVER_PATH"
    echo ""
    echo "To use:"
    echo "1. Copy to your EFI partition: EFI/OC/Drivers/"
    echo "2. Add to config.plist under UEFI -> Drivers"
    echo "3. Set LoadEarly: true (to run before other drivers)"
    echo ""

    # Copy to output
    cp "$DRIVER_PATH" "$SCRIPT_DIR/NvdaalFwsec.efi"
    echo "Also copied to: $SCRIPT_DIR/NvdaalFwsec.efi"
else
    echo "ERROR: Build failed - driver not found"
    exit 1
fi
