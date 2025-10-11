#pragma once

#include <math.h>
#include "../lib/constants.h"

typedef struct
{
	float alpha;
	float prev_sample;
	float gain;
} ResistorCapacitor;

void init_preemphasis(ResistorCapacitor *filter, float tau, float sample_rate, float ref_freq);
float apply_preemphasis(ResistorCapacitor *filter, float sample);