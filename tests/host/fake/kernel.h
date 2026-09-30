/* Host fake of UnixLib's <kernel.h>, for tests/host (as riscos-ffmpeg's).
   The registers are long: on the arm-linux test build that is 32 bits, as
   on RISC OS, so Wimp blocks can hold pointers. */
#ifndef FAKE_KERNEL_H
#define FAKE_KERNEL_H
typedef struct { long r[10]; } _kernel_swi_regs;
typedef struct { int errnum; char errmess[252]; } _kernel_oserror;
_kernel_oserror *_kernel_swi(int swi, _kernel_swi_regs *in, _kernel_swi_regs *out);
#endif
