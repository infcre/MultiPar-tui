/*
 * prefix.h -- force-included in front of every translation unit
 * (see the -include flag in Makefile).
 *
 * It provides the MSVC keywords and renames the handful of CRT/Win32 names
 * whose *semantics* differ between MSVC and glibc, so that the unmodified
 * MultiPar sources call the implementations in wincompat.c instead.
 *
 *  - printf family : glibc reads "%I64d" as "flag I, width 64, int";
 *                    MSVC means 64-bit.  w32_printf rewrites the format.
 *  - swprintf/fwprintf : MSVC wide printf reads "%s" as a wide string,
 *                    glibc reads it as a narrow one (and "%S" the other way
 *                    round).  w32_swprintf/w32_fwprintf translate.
 *  - threads/console/file helpers have no glibc equivalent at all.
 */
#ifndef PAR2J_COMPAT_PREFIX_H
#define PAR2J_COMPAT_PREFIX_H

#include "windows.h"

#include <assert.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <immintrin.h>	/* AVX2 declarations for gf16.c (enabled per-TU) */

/* ---------------------------------------------------- MSVC integer keywords */
/* phmd5*.c and the OpenCL headers use these without including <windows.h>,
 * which is why they must be defined here as well. */
#define __int8   char
#define __int16  short
#define __int32  int

/* ------------------------------------------------- MSVC cpuid intrinsics --- */
/* MSVC: void __cpuid(int regs[4], int leaf);
 *       void __cpuidex(int regs[4], int leaf, int subleaf);
 * Implemented with inline asm; <cpuid.h> is deliberately not used because its
 * __cpuid is a macro with a different argument order. */
static __inline__ void w32_cpuid(int regs[4], int leaf, int subleaf)
{
	unsigned int a, b, c, d;

	__asm__ volatile("cpuid"
	                 : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
	                 : "0"((unsigned int)leaf), "2"((unsigned int)subleaf));

	regs[0] = (int)a;
	regs[1] = (int)b;
	regs[2] = (int)c;
	regs[3] = (int)d;
}
#define __cpuid(regs, leaf)        w32_cpuid((regs), (int)(leaf), 0)
#define __cpuidex(regs, leaf, sub) w32_cpuid((regs), (int)(leaf), (int)(sub))

/* ------------------------------------------------------- CRT / API renames -- */
#define _beginthreadex   w32_beginthreadex
#define _endthreadex     w32_endthreadex

#define swprintf         w32_swprintf
#define wsprintf         w32_wsprintf
#define wsprintfA        w32_wsprintfA
#define fwprintf         w32_fwprintf
#define printf           w32_printf

#define _wfopen          w32_wfopen
#define _kbhit           w32_kbhit
#define _getch           w32_getch
#define _getche          w32_getche
#define _wcslwr          w32_wcslwr
#define _aligned_malloc  w32_aligned_malloc
#define _aligned_free    w32_aligned_free

/* The program entry point is wmain(); wincompat.c provides the real main()
 * and forwards to it.  Declare it so that par2_cmd.c's definition (which
 * omits the return type, old MSVC style) has a prior declaration. */
int wmain(int argc, wchar_t **argv);

#endif /* PAR2J_COMPAT_PREFIX_H */
