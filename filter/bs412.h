#pragma once

#ifdef DEBUG
#define BS412_DEBUG
#endif

#include <math.h>
#include <string.h>
#include <stdint.h>
#ifdef BS412_DEBUG
#include "../lib/debug.h"
#endif

typedef struct
{
	uint32_t mpx_deviation;
	uint32_t sample_rate;
	uint32_t sample_counter;
	float target;
	float attack;
	float release;
	float gain;
	double avg_power;
	double alpha;
	uint8_t can_compress : 1;
	uint8_t second_counter;
	float last_output;
} BS412Compressor;

// float dbr_to_deviation(float dbr);
float deviation_to_dbr(float deviation);

void init_bs412(BS412Compressor *mpx, uint32_t mpx_deviation, float target_power, float attack, float release, uint32_t sample_rate);
float bs412_compress(BS412Compressor *mpx, float audio, float sample_mpx);