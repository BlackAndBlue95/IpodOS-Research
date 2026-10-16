/* FreeType 2.14.3's anti-aliased rasterizer (ftgrays.c, FTL licence: docs/FTL.TXT in FreeType)
 * standing in for the OS's 2007-era one; mkpatch18.py --ft-raster points the smooth renderers at it. */
#define STANDALONE_
#define FALL_THROUGH  __attribute__((fallthrough))
#pragma GCC diagnostic ignored "-Wpointer-sign"
#include "ftgrays.c"

/* kept for the linker's section GC; mkpatch18.py reads the table's address from e.syms */
const void *const ft_grays_keep __attribute__((section(".rodata.keep"), used)) = &ft_grays_raster;
