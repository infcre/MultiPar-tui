/* versionhelpers.h -- common2.c only asks IsWindows7OrGreater() to decide
 * whether the CPUID leaf-7 feature test may run; the real decision is the
 * CPUID result itself, so simply answer "yes". */
#ifndef PAR2J_COMPAT_VERSIONHELPERS_H
#define PAR2J_COMPAT_VERSIONHELPERS_H

#define IsWindowsVersionOrGreater(major, minor, sp) (1)
#define IsWindows7OrGreater()  (1)
#define IsWindows8OrGreater()  (1)
#define IsWindows81OrGreater() (1)
#define IsWindows10OrGreater() (1)
#define IsWindowsServer()      (0)

#endif /* PAR2J_COMPAT_VERSIONHELPERS_H */
