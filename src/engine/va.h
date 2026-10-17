/* va (src/engine/va.c): the sub's shapes, shared by the engine, the encoder hook (src/slot/knobhook.c) and the screen
 * (src/slot/ui.c draw_va).
 *
 * Knob 7 (SHIFT + orange) holds the shape, 0..VA_SUB_SHAPES-1. va's range rows stay TE's plain type 0 (0..0x7FFF), so
 * TE never writes knobs 4..7 (SHIFT + blue / green / white stay its fine adjust of knobs 0..2) and every va sound saved
 * before the shapes holds 0 there: 0 is SQUARE, the sub va always had. The hook steps knob 7 one shape per call
 * (clamped, no wrap) and hands TE delta 0; any value outside 0..VA_SUB_SHAPES-1 plays SQUARE.
 */
#ifndef VA_H
#define VA_H
#include <stdint.h>

#define VA_SUB_KNOB 7
enum { SUB_SQUARE, SUB_SINE, SUB_TRI, SUB_SAW, SUB_PULSE, SUB_OCT2, VA_SUB_SHAPES };

static inline int va_sub_shape(int16_t k7)
{
    return (uint16_t)k7 < VA_SUB_SHAPES ? k7 : SUB_SQUARE;
}

#endif
