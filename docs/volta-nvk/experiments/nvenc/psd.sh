#!/bin/bash
# psd.sh setup.bin: print the NVENC H.264 picture setup (non-zero fields)
cd "$(dirname "$0")"
gdb -q -batch -x ps.gdb --args ./psdump "$1" 2>/dev/null | grep -v "^Breakpoint\|^\[\|^Using\|^$" | grep -v "= 0,$\|= 0$\|= 0 '\\\\000',\?$"
