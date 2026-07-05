#pragma once

#ifdef DEBUG
#define BS412_DEBUG
#endif

#include <math.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#ifdef BS412_DEBUG
#include "debug.h"
#endif

typedef struct {
	float reference;
	uint32_t sample_rate;
	uint32_t sample_counter;
	float target;
	float target_dbr;
	float attack;
	float release;
	float max_gain;
	float gain;
	double avg_power;
	float alpha;
	uint8_t can_compress : 1;
	uint8_t second_counter;
	float last_output;
	float gate_threshold;
	bool init;
	float knee_db;
	float strenght;
} BS412Compressor;

void init_bs412(BS412Compressor *comp, uint32_t mpx_deviation, float target_power, float attack, float release, float max_gain, float gate, float knee_db, float strenght, uint32_t sample_rate);
void reinit_bs412(BS412Compressor *comp, uint32_t mpx_deviation, float target_power, float attack, float release, float max_gain, float gate, float knee_db, float strenght);
float bs412_compress(BS412Compressor *comp, float audio, float sample_mpx, float* mpx_power);