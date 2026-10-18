/* Forced include for the host preview build of ui.c (preview.py). */
#include <stdint.h>
extern uint8_t host_palette[];
extern int16_t host_knobs[];
extern uint32_t host_prof[];
extern int32_t host_matrix[];
/* harm's RAM: harm_ram then the builder's scratch (ui_harm.h HARM_WORK), as on the device (preview_host.c) */
struct host_harm_s;
extern struct host_harm_s host_harm;
#define HARM_RAM ((struct harm_ram *)(void *)&host_harm)
/* the repeat screen's TE words (ui_repeat.h) */
extern uint8_t host_shift;
extern int32_t host_tape_speed;
#define TE_TAPE_SPEED host_tape_speed
#define WH_SHIFT host_shift
