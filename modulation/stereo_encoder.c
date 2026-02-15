#include "stereo_encoder.h"

// Multiplier is the multiplier to get to 19 khz
void init_stereo_encoder(StereoEncoder* st, uint8_t multiplier, Oscillator* osc, float audio_volume, float pilot_volume) {
    st->multiplier = multiplier;
    st->osc = osc;
    st->pilot_volume = pilot_volume;
    st->audio_volume = audio_volume;
    #ifdef STEREO_SSB
    init_delay_line(&st->delay_pilot, STEREO_SSB*2+1);
    init_delay_line(&st->delay, STEREO_SSB*2+1);
    #endif
}

float stereo_encode(StereoEncoder* st, uint8_t enabled, float left, float right, firhilbf *hilbert) {
    float mid = (left+right) * 0.5f;
    if(!enabled) return mid * st->audio_volume;
    #ifdef STEREO_SSB
    mid = delay_line(&st->delay, mid);
    #endif

    float half_audio = st->audio_volume * 0.5f;

    float side = (left-right) * 0.5f;

#ifdef STEREO_SSB
    float complex stereo_hilbert;
    firhilbf_r2c_execute(*hilbert, side, &stereo_hilbert);
    float signalx2cos = get_oscillator_cos_multiplier_ni(st->osc, st->multiplier * 2.0f);
#endif
    
    float signalx1 = get_oscillator_sin_multiplier_ni(st->osc, st->multiplier);
    #ifdef STEREO_SSB
    signalx1 = delay_line(&st->delay_pilot, signalx1);
    #endif
    float signalx2 = get_oscillator_sin_multiplier_ni(st->osc, st->multiplier * 2.0f);

#ifdef STEREO_SSB
    float stereo = (crealf(stereo_hilbert) * signalx2cos) + (cimagf(stereo_hilbert) * signalx2);
    return (mid*half_audio) + (signalx1*st->pilot_volume) + (stereo * half_audio);
#else
    return (mid*half_audio) + (signalx1*st->pilot_volume) + ((side*signalx2) * half_audio);
#endif
}

void exit_stereo_encoder(StereoEncoder* st) {
    exit_delay_line(&st->delay);
    exit_delay_line(&st->delay_pilot);
}