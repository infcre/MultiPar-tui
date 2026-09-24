/* conio.h -- MSVC console input, backed by wincompat.c (termios/select). */
#ifndef PAR2J_COMPAT_CONIO_H
#define PAR2J_COMPAT_CONIO_H

#include "windows.h"

int  w32_kbhit(void);
int  w32_getch(void);
int  w32_getche(void);

#endif /* PAR2J_COMPAT_CONIO_H */
