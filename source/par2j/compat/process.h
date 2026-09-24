/* process.h -- MSVC thread creation; the rename to w32_* happens in prefix.h. */
#ifndef PAR2J_COMPAT_PROCESS_H
#define PAR2J_COMPAT_PROCESS_H

#include "windows.h"

uintptr_t w32_beginthreadex(void *security, unsigned stack_size,
                            unsigned int (*start)(void *), void *arg,
                            unsigned initflag, unsigned *thrdaddr);
void      w32_endthreadex(unsigned retval);

#endif /* PAR2J_COMPAT_PROCESS_H */
