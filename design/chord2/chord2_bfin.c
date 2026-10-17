/* Blackfin compile check for the CHORD v2 draft (no libgcc / libc calls: only the three line primitives undefined):
 *   docker run --rm -v "$PWD/design/chord2:/work" op1-bfin sh -c \
 *     "bfin-elf-gcc -std=gnu99 -O2 -ffreestanding -fno-builtin -I/work -c /work/chord2_bfin.c -o /tmp/c2.o && bfin-elf-nm -u /tmp/c2.o"
 */
#include <stdint.h>
typedef int32_t fp32;
#define FP(x) ((fp32)(x) << 10)
enum { RED = 2, OFFRED = 3, OFFGREY = 4, DARKGREEN = 5, PURPLEGREY = 8, LIGHTBLUE = 9, DARKBLUE = 10, GREY = 12, ORANGE = 13, WHITE = 14, BLUE = 15, GREEN = 16, LIGHTGREEN = 18 };
extern void line_fp(void *r, fp32 x0, fp32 y0, fp32 x1, fp32 y1, fp32 w, int colour);
extern void line_fast(void *r, fp32 x0, fp32 y0, fp32 x1, fp32 y1, int colour);
extern void vline_fast(void *r, fp32 x, fp32 y0, fp32 y1, int colour);
#include "chord2_ui.h"
#include "chord2_factory.h"
struct c2_ui g_u;
void entry(void *r);
void entry(void *r) { c2_page_map(r, &g_u); c2_page_fill(r, &g_u, g_u.keys); c2_page_shape(r, &g_u); c2_badge(r,&g_u,"G7"); c2_chip(r,&g_u,"G7",0);
  c2_load(g_u.keys, c2_f_neo, 3); int o; int8_t p[4]={0}; o = c2_smooth(&g_u.keys[0], 0, p, 4, 7); c2_structure(&g_u.keys[1], 3); (void)o; }
