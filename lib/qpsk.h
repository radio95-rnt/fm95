#pragma once

#include <liquid/liquid.h>
#include "oscillator.h"
#include "bit_ring.h"

typedef struct {
	Oscillator    carrier;        // IF carrier (e.g. 3 kHz)
	firfilt_rrrf  fir_i, fir_q;   // liquid RRC matched-filter shapers
	float        *q_delay;
	int           q_delay_len;
	int           q_delay_pos;
	int           sps;            // samples per symbol
	int           symbol_clock;
	float         sym_i, sym_q;
	bit_ring_t   *bitring;
	uint8_t       last_bits[2];   // hold last symbol if bitring runs dry
} OQPSKMod;

void  oqpsk_init(OQPSKMod *m, float if_freq, float sample_rate, float symbol_rate, float alpha, bit_ring_t *bitring);
float oqpsk_process(OQPSKMod *m);
void  oqpsk_destroy(OQPSKMod *m);