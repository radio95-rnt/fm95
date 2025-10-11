#include "iir.h"

void init_preemphasis(ResistorCapacitor *filter, float tau, float sample_rate, float ref_freq) {
    float dt = 1.0f / sample_rate;
    filter->alpha = tau / (tau + dt);

    float omega = M_2PI * ref_freq / sample_rate;
    filter->gain = 1.0f / sqrtf(1.0f + filter->alpha * filter->alpha - 2.0f * filter->alpha * cosf(omega));

    filter->prev_sample = 0.0f;
}
inline float apply_preemphasis(ResistorCapacitor *filter, float sample) {
	float out = (sample - filter->alpha * filter->prev_sample) * filter->gain;
	filter->prev_sample = sample;
	return out;
}