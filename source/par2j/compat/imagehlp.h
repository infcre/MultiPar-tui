/* imagehlp.h -- only CheckSumMappedFile() is needed (par2.c self-test).
 * The PE checksum cannot exist for an ELF file, so wincompat.c reports the
 * header and file sums as equal and lets the caller fall through to its own
 * CRC check (which then reports the expected mismatch). */
#ifndef PAR2J_COMPAT_IMAGEHLP_H
#define PAR2J_COMPAT_IMAGEHLP_H

#include "windows.h"

PIMAGE_NT_HEADERS CheckSumMappedFile(void *base, unsigned int length,
                                     unsigned int *header_sum,
                                     unsigned int *file_sum);

#endif /* PAR2J_COMPAT_IMAGEHLP_H */
