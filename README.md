# busybox_apt

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

1.  Place the `busybox_apt` directory inside your BusyBox source tree.
2.  Apply the provided patch to integrate the applet into the root configuration:
    ```bash
    patch -p0 < busybox_apt/busybox_apt.patch
    ```
3.  Alternatively, use the automated build script. This script is smart enough to be run from the BusyBox root **or** from within the `busybox_apt` directory:
    ```bash
    ./busybox_apt/busybox_build.sh
    ```
4.  Enable `APT` in the configuration menu (under "Applets") if using manual config:
    ```bash
    make menuconfig
    ```

## Development and Stability

This project has been developed with a focus on professional standards and robustness. It correctly handles version constraints in dependencies and performs batch installations to ensure circular dependencies are handled safely by `dpkg`.

---

*I hope this tool enhances your BusyBox experience. Contributions and feedback are welcome!*
