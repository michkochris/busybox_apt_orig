# busybox_apt

Please see the LICENSE file for details on copying and usage.

What is busybox_apt:

  `busybox_apt` is a high-level package management frontend. It is a
  lightweight interface to `dpkg` which handles repository management
  and dependency resolution. It provides minimalist replacements for
  the features found in the Advanced Package Tool (APT).

  Built with size-optimization and system recovery in mind, `busybox_apt`
  integrates directly into the BusyBox Kbuild system and utilizes internal
  applets for critical operations. This makes it a robust choice for embedded
  systems, rescue disks, and minimalist Debian-based environments where a
  full `apt` installation is too heavy or when the system manager is broken.

----------------

Features:

  * Professional UI: Familiar output format with "Get:N", "Hit:N", and real-time
    progress tracking via a terminal-aware progress bar.
  * Dependency Resolution: Automatically resolves recursive dependencies,
    including support for Virtual Packages (via `Provides:`).
  * System Rescue Toolkit: Specialized commands like `reinstall`, `verify`,
    and `rescue-install` for repairing corrupted systems.
  * Disk Usage Reporting: Accurate calculation of disk space used or freed
    after operations, tracking individual package installed-sizes.
  * Manual Extraction: `rescue-install` bypasses broken `dpkg` instances by
    using internal `ar` and `tar` applets to extract payloads directly to `/`.
  * Debian-Style Versioning: Robust comparison logic supporting epochs,
    revisions, and the `~` sorting character.
  * Lightweight Defaults: To respect system resources, Recommended packages
    are listed but NOT installed by default.

----------------
Using busybox_apt:
```text
  Usage: apt [-f] COMMAND [PACKAGE...]

  High-level package management frontend

  Options:
      -f, --fix-broken    Pass --force-depends to dpkg

  Commands:
      update              Update list of available packages
      install             Install new packages
      remove              Remove packages
      upgrade             Upgrade the system
      reinstall           Reinstall packages (restores files)
      rescue-install      Install packages bypassing dpkg (uses internal ar/tar to /)
      verify              Verify package sanity (check status, deps, and files)
      md5check            Verify package integrity (checks md5sums)
      list --upgradable   Show packages with available updates
      search              Search for a package (alphabetically sorted)
```
  Examples:

    $ ./busybox apt update
    $ ./busybox apt install nano
    $ ./busybox apt verify bash
    $ ./busybox apt md5check coreutils
    $ ./busybox apt rescue-install libc6

----------------

Rescue Workflow:

  `busybox_apt` is uniquely suited for system recovery. When a critical
  library or the package manager itself is corrupted:

  1. Use `verify` to identify broken dependencies, missing files, or
     dangling symlinks.
  2. Use `md5check` to perform a bit-level validation of installed files.
  3. Use `reinstall` to restore files via standard `dpkg` paths.
  4. Use `rescue-install` as a failsafe to extract critical packages
     directly to the root filesystem using BusyBox internal applets.

----------------

Build Instructions:

  To integrate `busybox_apt` into your BusyBox build:

  1. Clone this repository into the root of your BusyBox source tree:
     $ git clone https://github.com/michkochris/busybox_apt_orig busybox_apt

  2. Automated Build:
     The included script will integrate the applet, configure, and compile:
     $ ./busybox_apt/busybox_build.sh

  3. Manual Integration:
     $ patch -p0 < busybox_apt/busybox_apt.patch
     $ make menuconfig  # Enable 'apt' under 'Applets' -> 'Busybox APT'
     $ make

----------------

License:

  This project is licensed under the GNU General Public License, version 2
  (GPLv2), matching the license of the BusyBox project.

----------------

Contact:

  For feedback, bug reports, or inquiries:
  michkochris@gmail.com | runepkg@gmail.com
