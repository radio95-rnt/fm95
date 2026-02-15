#include "delay.h"

void init_delay_line(delay_line_t *delay_line, uint32_t sample_rate) {
	delay_line->buffer = malloc(sample_rate * sizeof(float));
	memset(delay_line->buffer, 0, sample_rate * sizeof(float));
}

void set_delay_line(delay_line_t *delay_line, uint32_t new_delay) {
	delay_line->delay = new_delay;
}

float delay_line(delay_line_t *delay_line, float in) {
	delay_line->buffer[delay_line->idx++] = in;
	if (delay_line->idx >= delay_line->delay) delay_line->idx = 0;
	return delay_line->buffer[delay_line->idx];
}

void exit_delay_line(delay_line_t *delay_line) {
	if(delay_line->buffer != NULL) free(delay_line->buffer);
	delay_line->buffer = NULL;
}