#!/bin/bash
# clean_up.sh - Cleanup script for busybox_apt

echo "Cleaning up busybox_apt temporary files..."

rm -f *.o *.a
rm -f built-in.o
rm -f .*.cmd
rm -f busybox_apt.patch
rm -f *Packages.gz

echo "Cleanup complete."
