#!/bin/sh
# check-unixlib.sh ELF... - each program was linked with UnixLib 5.0.1 or
# later (the thread timer that can run from the PThreadTicker module), and
# a program with FFmpeg's file protocol with UnixLib 5.0.2's files over
# 2GB: its libavformat built against 5.0.2's headers calls
# __unixlib_fstat64 (the 64-bit st_size), not the old fstat64.
#
# UnixLib 5.0.3.1's (and 5.0.3.2's: its changes are LLONG_MIN,
# getservbyname_r and eventfd, none of which Matinee links) start-up code claims a 640-byte pthread block in the
# RMA (the ticker code and its counters; 5.0.1 to 5.0.3.1-rc8 claimed 472,
# the Warzone toolchain's earlier UnixLib 248, GCCSDK's own 120). A program linked with the wrong
# library would still run, but without the fixes the apps' !Run files load
# PThreadTicker for. Same test as riscos-unixlib's tools/check-lib.sh:
# the "mov r3, #<size>" before OS_Module 6 in no_dynamic_area.
# And (REQUIRE, default __exit_status) a symbol the UnixLib wanted has:
# __exit_status came with 5.0.3.1-rc7 (_exit(n) exits with n; rc8's
# library is the same). Run by build/build.sh.
CROSS=${CROSS:-/root/gccsdk/env/bin/arm-riscos-gnueabihf-}
EXPECTED=${EXPECTED:-640}
REQUIRE=${REQUIRE:-__exit_status}
WANT=${WANT:-UnixLib 5.0.3.2}
bad=0
for f in "$@"; do
  if ! ${CROSS}nm "$f" 2>/dev/null | grep -q ' __pthread_call_every_code$'; then
    echo "$f: not linked with a UnixLib that has the ticker fix" >&2; bad=1; continue
  fi
  size=$(${CROSS}objdump -d "$f" | awk '/<no_dynamic_area>:$/{p=1;next} p&&/^$/{exit}
    p&&/mov\tr3, #/{s=$0} p&&/svc\t0x0002001e/{sub(/.*#/,"",s); sub(/[ \t;].*/,"",s); print s; exit}')
  syms=$(${CROSS}nm "$f" 2>/dev/null)
  if echo "$syms" | grep -q ' ff_file_protocol$' && ! echo "$syms" | grep -q ' __unixlib_fstat64$'; then
    echo "$f: FFmpeg's file protocol without UnixLib 5.0.2's large files: rebuild libavformat against 5.0.2" >&2; bad=1; continue
  fi
  if [ -n "$REQUIRE" ] && ! echo "$syms" | grep -q " $REQUIRE\$"; then
    echo "$f: no $REQUIRE: relink with $WANT" >&2; bad=1; continue
  fi
  if [ "$size" = "$EXPECTED" ]; then
    echo "  $(basename "$f"): $WANT (the $EXPECTED-byte pthread block$(echo "$syms" | grep -q ' __unixlib_fstat64$' && echo ', files over 2GB'))"
  else
    echo "$f: claims a ${size:-?}-byte pthread block, not $EXPECTED: relink with $WANT" >&2; bad=1
  fi
done
exit $bad
