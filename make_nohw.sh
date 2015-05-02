#!/bin/sh

set -x

if [ "$1" = "clean" ]; then
   find . -name "*.ko" -type f -exec rm {} \;
   find . -name "*.o" -type f -exec rm {} \;
   find . -name ".*.ko.cmd" -type f -exec rm {} \;
   find . -name ".*.o.cmd" -type f -exec rm {} \;
   find . -name "*.mod.c" -type f -exec rm {} \;
   rm modules.order Module.symvers
   rm -fr .tmp_versions
else
   set -e
   set -u

   make -C "/lib/modules/$(uname -r)/build/" "V=1" "SUBDIRS=$(pwd)" modules
fi

