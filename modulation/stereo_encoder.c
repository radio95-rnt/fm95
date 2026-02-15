#include "stereo_encoder.h"

// Multiplier is the multiplier to get to 19 khz
void init_stereo_encoder(StereoEncoder* st, uint8_t stereo_ssb, uint8_t multiplier, Oscillator* osc, float audio_volume, float pilot_volume) {
    st->multiplier = multiplier;
    st->osc = osc;
    st->pilot_volume = pilot_volume;
    st->audio_volume = audio_volume;
    if(stereo_ssb) {
        init_delay_line(&st->delay_pilot, stereo_ssb*2+1);
        init_delay_line(&st->delay, stereo_ssb*2+1);
        st->stereo_hilbert = firhilbf_create(stereo_ssb, 60);
    } else st->stereo_hilbert = NULL;
}

float stereo_encode(StereoEncoder* st, uint8_t enabled, float left, float right) {
    float mid = (left+right) * 0.5f;
    if(!enabled) return mid * st->audio_volume;

    if(st->stereo_hilbert) mid = delay_line(&st->delay, mid);

    float half_audio = st->audio_volume * 0.5f;

    float side = (left-right) * 0.5f;

    float complex stereo_hilbert = 0+0*I;
    if(st->stereo_hilbert) firhilbf_r2c_execute(st->stereo_hilbert, side, &stereo_hilbert);
    
    float signalx1 = get_oscillator_sin_multiplier_ni(st->osc, st->multiplier);

    float signalx2cos = 0.0f;
    if(st->stereo_hilbert) {
        signalx1 = delay_line(&st->delay_pilot, signalx1);
        signalx2cos = get_oscillator_cos_multiplier_ni(st->osc, st->multiplier * 2.0f);
    }
    float signalx2 = get_oscillator_sin_multiplier_ni(st->osc, st->multiplier * 2.0f);

    if(st->stereo_hilbert) {
        float stereo = (crealf(stereo_hilbert) * signalx2cos) + (cimagf(stereo_hilbert) * signalx2);
        return (mid*half_audio) + (signalx1*st->pilot_volume) + (stereo * half_audio);
    } else {
        return (mid*half_audio) + (signalx1*st->pilot_volume) + ((side*signalx2) * half_audio);
    }
}

void exit_stereo_encoder(StereoEncoder* st) {
    if(st->stereo_hilbert) {
        exit_delay_line(&st->delay);
        exit_delay_line(&st->delay_pilot);
	    firhilbf_destroy(st->stereo_hilbert);
        st->stereo_hilbert = NULL;
    }
}