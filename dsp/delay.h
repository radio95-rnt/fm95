#include <string.h>
#include <stdlib.h>
#include <stdint.h>

typedef struct delay_line_t {
	float *buffer;
	uint32_t delay;
	uint32_t idx;
} delay_line_t;

void init_delay_line(delay_line_t *dl, uint32_t delay_samples);
float delay_line(delay_line_t *dl, float in);
void exit_delay_line(delay_line_t *dl);
