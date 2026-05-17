#!/usr/bin/env bash

apt update
#apt install fastfetch
# apt install linux-headers-$(uname -r)
apt install binutils bison gawk gcc g++ make texinfo xz-utils libtool-bin linux-headers-amd64

ln -sf bash /bin/sh
