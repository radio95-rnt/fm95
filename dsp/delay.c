#include <stdlib.h>
#include <string.h>
#include "delay.h"

void init_delay_line(delay_line_t *dl, uint32_t delay_samples) {
    dl->delay = delay_samples;
    dl->idx = 0;
    dl->buffer = calloc(delay_samples, sizeof(float));
}

float delay_line(delay_line_t *dl, float in) {
    float out = dl->buffer[dl->idx];   // read delayed sample

    dl->buffer[dl->idx] = in;          // write new input

    dl->idx++;
    if (dl->idx >= dl->delay) dl->idx = 0;

    return out;
}

void exit_delay_line(delay_line_t *dl) {
    if(dl->buffer != NULL) free(dl->buffer);
    dl->buffer = NULL;
}