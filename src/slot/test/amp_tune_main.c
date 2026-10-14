/* Host driver for the amp tuner (src/slot/amp_tune.h): reads windows of AMP_TUNE_N int16 samples from stdin and
 * prints amp_tune_pitch() for each (MIDI note Q16, -1 unpitched). amp_tune_check.py builds and drives it. */
#include <stdio.h>
#include "amp_tune.h"

static struct amp_tune_work work;

int main(void)
{
    static int16_t x[AMP_TUNE_N];
    while (fread(x, 2, AMP_TUNE_N, stdin) == AMP_TUNE_N)
        printf("%d\n", (int)amp_tune_pitch(x, &work));
    return 0;
}
