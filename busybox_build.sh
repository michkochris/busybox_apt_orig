#!/bin/bash
# busybox_build.sh - Automated BusyBox build with APT support

set -e

# Detect if we are inside the busybox_apt directory or at the root
if [ -f "Makefile" ] && [ -d "busybox_apt" ]; then
    ROOT_DIR="."
elif [ -f "../Makefile" ] && [ -d "../busybox_apt" ]; then
    ROOT_DIR=".."
else
    echo "Error: Could not find BusyBox root directory."
    echo "Please run this script from the BusyBox root or from the 'busybox_apt' subdirectory."
    exit 1
fi

cd "$ROOT_DIR"

echo "Starting BusyBox build process in $(pwd)..."

# 0. Integrate into build system if not already done
if ! grep -q "busybox_apt/Config.in" Config.in; then
    echo "Integrating APT into Config.in..."
    # Try to append after sysklogd, otherwise just append to the end
    if grep -q "sysklogd/Config.in" Config.in; then
        sed -i '/sysklogd\/Config.in/a source busybox_apt/Config.in' Config.in
    else
        echo "source busybox_apt/Config.in" >> Config.in
    fi
fi

if ! grep -q "busybox_apt/" Makefile; then
    echo "Integrating APT into Makefile..."
    # Try to add before sysklogd
    if grep -q "sysklogd/" Makefile; then
        sed -i 's|sysklogd/|busybox_apt/ \\\n\t\tsysklogd/|' Makefile
    else
        sed -i '/libs-y/ s/$/ busybox_apt\//' Makefile
    fi
fi

# WSL Detection and Fixes
if grep -qi "microsoft" /proc/version; then
    echo "Detected WSL environment. Applying performance and permission tweaks..."
fi

# 1. Generate default configuration
echo "Generating default configuration..."
make defconfig

# 2. Enable APT applet
echo "Configuring APT applet..."
sed -i 's/^# CONFIG_APT is not set/CONFIG_APT=y/' .config
if ! grep -q "^CONFIG_APT=y" .config; then
    echo "CONFIG_APT=y" >> .config
fi

# 3. Ensure dependencies are met
sed -i 's/^# CONFIG_WGET is not set/CONFIG_WGET=y/' .config
sed -i 's/^# CONFIG_DPKG is not set/CONFIG_DPKG=y/' .config
sed -i 's/^# CONFIG_GZIP is not set/CONFIG_GZIP=y/' .config
sed -i 's/^# CONFIG_ZCAT is not set/CONFIG_ZCAT=y/' .config
sed -i 's/^# CONFIG_FEATURE_SEAMLESS_GZ is not set/CONFIG_FEATURE_SEAMLESS_GZ=y/' .config

# Disable features that might fail to compile due to kernel header mismatches
echo "Disabling problematic features..."
sed -i 's/CONFIG_FEATURE_COMPRESS_USAGE=y/# CONFIG_FEATURE_COMPRESS_USAGE is not set/' .config
sed -i 's/CONFIG_TC=y/# CONFIG_TC is not set/' .config

# 4. Finalize configuration
make silentoldconfig

# 5. Compile
echo "Compiling BusyBox..."
if make -j$(nproc); then
    # On WSL/Windows mounts, chmod +x might fail but the bit is often set anyway.
    if grep -qi "microsoft" /proc/version; then
        chmod a+x busybox || true
    else
        chmod a+x busybox
    fi
    echo "--------------------------------------------------"
    echo "Build successful!"
    echo "The 'busybox' binary has been created in the current directory."
    echo "You can test the new applet using: ./busybox apt"
    echo "--------------------------------------------------"
else
    echo "--------------------------------------------------"
    echo "Error: Build failed!"
    echo "Please check the compilation errors above."
    echo "--------------------------------------------------"
    exit 1
fi
