# busybox_apt

**Stable Release**

`busybox_apt` is a lightweight, professional implementation of the Advanced Package Tool (APT) designed specifically as an applet for the BusyBox environment. It provides a high-level frontend to `dpkg`, handling repository management, dependency resolution, and automated updates with a UI that closely mimics standard Debian `apt`.

## Features

- **Professional UI**: Familiar output format with "Get:N", "Hit:N", and colored highlights.
- **Progress Tracking**: Real-time progress bar at the bottom of the terminal during installations.
- **Repository Management**: Supports standard Debian-style `sources.list` files.
- **Dependency Resolution**: Automatically resolves and fetches recursive dependencies.
- **Safety Checks**: Confirmation prompts and download size calculations before any changes.
- **Optimized for BusyBox**: Minimal footprint, written in C and fully integrated into the Kbuild system.

## Usage

```
Usage: apt COMMAND [PACKAGE...]

High-level package manager

Commands:
    update      Update list of available packages
    install     Install new packages
    remove      Remove packages
    upgrade     Upgrade the system by installing/upgrading packages
    search      Search for a package
```

### Examples

- **Update package lists**: `./busybox apt update`
- **Search for a package**: `./busybox apt search nano`
- **Install a package**: `./busybox apt install nano`
- **Upgrade all packages**: `./busybox apt upgrade`

## Build Instructions

To integrate `busybox_apt` into your BusyBox build, follow these steps:

1.  **Download and extract BusyBox**:
    ```bash
    wget https://busybox.net/downloads/busybox-1.37.0.tar.bz2
    tar -xvjf busybox-1.37.0.tar.bz2
    cd busybox-1.37.0
    ```

2.  **Clone this repository**:
    Clone the `busybox_apt` tool into the root of your BusyBox source tree:
    ```bash
    git clone https://github.com/michkochris/busybox_apt_orig busybox_apt
    ```
    *(Note: Ensure the directory is named `busybox_apt` as the build system expects this path.)*

3.  **Build automatically**:
    The easiest way to build is using the included automated script. **This script will automatically integrate the applet into the BusyBox build system for you**, configure it, and compile:
    ```bash
    ./busybox_apt/busybox_build.sh
    ```

4.  **Alternative: Manual Integration**:
    If you prefer to integrate things yourself, use the provided patch file:
    ```bash
    patch -p0 < busybox_apt/busybox_apt.patch
    make menuconfig  # Enable 'apt' under 'Applets' -> 'Busybox APT'
    make
    ```

## License

This project is licensed under the **GNU General Public License, version 2 (GPLv2)**, matching the license of the BusyBox project. See the `LICENSE` file for the full text.

---

## **Contact**
For feedback, bug reports, or inquiries, reach out at:
[michkochris@gmail.com](mailto:michkochris@gmail.com) | [runepkg@gmail.com](mailto:runepkg@gmail.com)
