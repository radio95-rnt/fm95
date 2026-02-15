#include "stereo_encoder.h"

#ifdef STEREO_SSB
static void init_delay_line(struct delay_line_t *delay_line, uint32_t sample_rate) {
	delay_line->buffer = malloc(sample_rate * sizeof(float));
	memset(delay_line->buffer, 0, sample_rate * sizeof(float));
}

static void set_delay_line(struct delay_line_t *delay_line, uint32_t new_delay) {
	delay_line->delay = new_delay;
}

static inline float delay_line(struct delay_line_t *delay_line, float in) {
	delay_line->buffer[delay_line->idx++] = in;
	if (delay_line->idx >= delay_line->delay) delay_line->idx = 0;
	return delay_line->buffer[delay_line->idx];
}

static void exit_delay_line(struct delay_line_t *delay_line) {
	free(delay_line->buffer);
}
#endif

// Multiplier is the multiplier to get to 19 khz
void init_stereo_encoder(StereoEncoder* st, uint8_t multiplier, Oscillator* osc, float audio_volume, float pilot_volume) {
    st->multiplier = multiplier;
    st->osc = osc;
    st->pilot_volume = pilot_volume;
    st->audio_volume = audio_volume;
#ifdef STEREO_SSB
    init_delay_line(&st->delay, 15); // 7*2+1
#endif
}

#ifdef STEREO_SSB
float stereo_encode(StereoEncoder* st, uint8_t enabled, float left, float right, firhilbf *hilbert) {
#else
float stereo_encode(StereoEncoder* st, uint8_t enabled, float left, float right) {
#endif
    float mid = (left+right) * 0.5f;
    if(!enabled) return mid * st->audio_volume;
    mid = delay_line(&st->delay, mid);

    float half_audio = st->audio_volume * 0.5f;

    float side = (left-right) * 0.5f;

#ifdef STEREO_SSB
    float complex stereo_hilbert;
    firhilbf_r2c_execute(*hilbert, side, &stereo_hilbert);
    float signalx2cos = get_oscillator_cos_multiplier_ni(st->osc, st->multiplier * 2.0f);
#endif
    
    float signalx1 = get_oscillator_sin_multiplier_ni(st->osc, st->multiplier);
    float signalx2 = get_oscillator_sin_multiplier_ni(st->osc, st->multiplier * 2.0f);

#ifdef STEREO_SSB
    float stereo = (crealf(stereo_hilbert) * signalx2cos) - (cimagf(stereo_hilbert) * signalx2);
    return (mid*half_audio) + (signalx1*st->pilot_volume) + (stereo * half_audio);
#else
    return (mid*half_audio) + (signalx1*st->pilot_volume) + ((side*signalx2) * half_audio);
#endif
}