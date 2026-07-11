#include "qpsk.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define OQPSK_RRC_DELAY 6 // filter delay in symbols (liquid's "m" param)

static void oqpsk_next_dibit(OQPSKMod *m, float *i, float *q) {
	uint8_t b0 = m->last_bits[0], b1 = m->last_bits[1], bit;
	if (bit_ring_read1(m->bitring, &bit)) b0 = bit;
	if (bit_ring_read1(m->bitring, &bit)) b1 = bit;
	m->last_bits[0] = b0;
	m->last_bits[1] = b1;
	*i = b0 ? -1.0f : 1.0f;
	*q = b1 ? -1.0f : 1.0f;
}

void oqpsk_init(OQPSKMod *m, float if_freq, float sample_rate, float symbol_rate, float alpha, bit_ring_t *bitring) {
	memset(m, 0, sizeof(*m));
	init_oscillator(&m->carrier, if_freq, sample_rate);

	m->sps = (int)roundf(sample_rate / symbol_rate);
	if (m->sps < 2) m->sps = 2;
	if (m->sps % 2 != 0) m->sps += 1; // must be even for the half-symbol Q delay

	m->fir_i = firfilt_rrrf_create_rnyquist(LIQUID_FIRFILT_RRC, m->sps, OQPSK_RRC_DELAY, alpha, 0.0f);
	m->fir_q = firfilt_rrrf_create_rnyquist(LIQUID_FIRFILT_RRC, m->sps, OQPSK_RRC_DELAY, alpha, 0.0f);

	m->q_delay_len = m->sps / 2;
	m->q_delay = calloc(m->q_delay_len, sizeof(float));
	m->q_delay_pos = 0;
	m->symbol_clock = 0;
	m->sym_i = m->sym_q = 1.0f;
	m->bitring = bitring;
}

float oqpsk_process(OQPSKMod *m) {
	float impulse_i = 0.0f, impulse_q = 0.0f;

	if (m->symbol_clock == 0) {
		oqpsk_next_dibit(m, &m->sym_i, &m->sym_q);
		impulse_i = m->sym_i * (float)m->sps;
		impulse_q = m->sym_q * (float)m->sps;
	}
	m->symbol_clock = (m->symbol_clock + 1) % m->sps;

	float filt_i, filt_q_raw;
	firfilt_rrrf_push(m->fir_i, impulse_i);
	firfilt_rrrf_execute(m->fir_i, &filt_i);
	firfilt_rrrf_push(m->fir_q, impulse_q);
	firfilt_rrrf_execute(m->fir_q, &filt_q_raw);

	float filt_q = m->q_delay[m->q_delay_pos];
	m->q_delay[m->q_delay_pos] = filt_q_raw;
	m->q_delay_pos = (m->q_delay_pos + 1) % m->q_delay_len;

	float c = get_oscillator_cos_sample(&m->carrier);
	float s = get_oscillator_sin_sample(&m->carrier);
	return filt_i * c - filt_q * s;
}

void oqpsk_destroy(OQPSKMod *m) {
	firfilt_rrrf_destroy(m->fir_i);
	firfilt_rrrf_destroy(m->fir_q);
	free(m->q_delay);
}