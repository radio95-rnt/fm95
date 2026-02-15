#pragma once

#define STEREO_SSB 12

#include <stdint.h>
#include "../dsp/oscillator.h"
#include <liquid/liquid.h>
#include <complex.h>
#include "../dsp/delay.h"

typedef struct
{
    uint8_t multiplier;
    Oscillator* osc;
    float audio_volume;
    float pilot_volume;
    delay_line_t delay;
    delay_line_t delay_pilot;
} StereoEncoder;

void init_stereo_encoder(StereoEncoder *st, uint8_t multiplier, Oscillator *osc, float audio_volume, float pilot_volume);
float stereo_encode(StereoEncoder* st, uint8_t enabled, float left, float right, firhilbf *hilbert);
void exit_stereo_encoder(StereoEncoder* st);