#pragma once

#include "constants.h"
#include <stdbool.h>
#include <math.h>

typedef struct {
	float phase;
	double unwrapped_phase;
	float phase_increment;
	float sample_rate;
} Oscillator;

void init_oscillator(Oscillator *osc, float frequency, float sample_rate);
void change_oscillator_frequency(Oscillator *osc, float frequency);
float get_oscillator_sin_sample(Oscillator *osc);
float get_oscillator_cos_sample(Oscillator *osc);
float get_oscillator_sin_multiplier_ni(Oscillator *osc, float multiplier);
float get_oscillator_cos_multiplier_ni(Oscillator *osc, float multiplier);
bool advance_oscillator(Oscillator *osc);
bool oscillator_did_cycle(Oscillator *osc, float phase_shift, float *prev_shifted_phase);