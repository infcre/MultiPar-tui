/* intrin.h -- MSVC intrinsics umbrella.  On MSVC this header pulls in all of
 * the x86 intrinsic headers, and gf16.c relies on that.  The __cpuid /
 * __cpuidex intrinsics come from prefix.h (force-included); SIMD paths are
 * enabled per translation unit through the Makefile. */
#ifndef PAR2J_COMPAT_INTRIN_H
#define PAR2J_COMPAT_INTRIN_H

#include "windows.h"

#include <emmintrin.h>
#include <tmmintrin.h>
#include <smmintrin.h>
#include <nmmintrin.h>
#include <wmmintrin.h>

#endif /* PAR2J_COMPAT_INTRIN_H */
