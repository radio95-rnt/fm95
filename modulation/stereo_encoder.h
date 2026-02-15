#pragma once

#define STEREO_SSB

#include <stdint.h>
#include "../dsp/oscillator.h"
#ifdef STEREO_SSB
#include <liquid/liquid.h>
#include <complex.h>
#include <string.h>

typedef struct delay_line_t {
	float *buffer;
	uint32_t delay;
	uint32_t idx;
} delay_line_t;
#endif

typedef struct
{
    uint8_t multiplier;
    Oscillator* osc;
    float audio_volume;
    float pilot_volume;
#ifdef STEREO_SSB
    struct delay_line_t delay;
#endif
} StereoEncoder;

void init_stereo_encoder(StereoEncoder *st, uint8_t multiplier, Oscillator *osc, float audio_volume, float pilot_volume);

#ifdef STEREO_SSB
float stereo_encode(StereoEncoder* st, uint8_t enabled, float left, float right, firhilbf hilbert);
#else
float stereo_encode(StereoEncoder* st, uint8_t enabled, float left, float right);
#endif
