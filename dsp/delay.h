#include <string.h>

typedef struct delay_line_t {
	float *buffer;
	uint32_t delay;
	uint32_t idx;
} delay_line_t;

void init_delay_line(delay_line_t *delay_line, uint32_t sample_rate);
void set_delay_line(delay_line_t *delay_line, uint32_t new_delay);
float delay_line(delay_line_t *delay_line, float in);
void exit_delay_line(delay_line_t *delay_line);
