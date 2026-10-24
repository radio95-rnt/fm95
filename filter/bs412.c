#include "bs412.h"

#define BS412_TIME 60
#define CLAMP(x, lo, hi) (((x) < (lo)) ? (lo) : ((x) > (hi) ? (hi) : (x)))

inline float power_to_dbr(float power, float ref) {
    if (power < 1e-12f) return -100.0f;
    return 10*log10f(power / ref);
}

void init_bs412(BS412Compressor* comp, uint32_t mpx_deviation, float target_power, float attack, float release, float max_gain, float gate, float knee_db, float strenght, uint32_t sample_rate) {
	comp->reference = (19000.0f / mpx_deviation) * ((19000.0f / mpx_deviation) * 0.5f);
	comp->avg_power = 0.0f;
    comp->alpha = 1.0f - expf(-1.0f / (BS412_TIME * sample_rate));
	comp->sample_rate = sample_rate;
	comp->attack = expf(-1.0f / (attack * sample_rate));
	comp->release = expf(-1.0f / (release * sample_rate));
	comp->target = comp->reference * powf(10.0f, target_power / 10.0f);
	comp->target_dbr = power_to_dbr(comp->target, comp->reference);
	comp->gain = 1.0f;
	comp->max_gain = max_gain;
	comp->gate_threshold = comp->reference * powf(10.0f, gate / 10.0f);
	comp->knee_db = knee_db;
	comp->init = true;
	comp->strenght = strenght;
}

void reinit_bs412(BS412Compressor* comp, uint32_t mpx_deviation, float target_power, float attack, float release, float max_gain, float gate, float knee_db, float strenght) {
	comp->reference = (19000.0f / mpx_deviation) * ((19000.0f / mpx_deviation) * 0.5f);
	comp->target = comp->reference * powf(10.0f, target_power / 10.0f);
	comp->target_dbr = power_to_dbr(comp->target, comp->reference);
	comp->gate_threshold = comp->reference * powf(10.0f, gate / 10.0f);
	comp->attack = expf(-1.0f / (attack * comp->sample_rate));
	comp->release = expf(-1.0f / (release * comp->sample_rate));
	comp->max_gain = max_gain;
	comp->knee_db = knee_db;
	comp->strenght = strenght;
}

float bs412_compress(BS412Compressor* comp, float audio, float sample_mpx, float* mpx_power) {
	float combined = audio + sample_mpx;
	float gained_audio = audio * comp->gain;
	float output_sample = gained_audio + sample_mpx;

	float w = (gained_audio * gained_audio - comp->gate_threshold) / comp->gate_threshold;
	comp->avg_power += comp->alpha * CLAMP(w, 0.0f, 1.0f) * (output_sample * output_sample - comp->avg_power);

	float level_dbr = power_to_dbr(fmaxf(comp->avg_power, 1e-12f), comp->reference);
	float half_knee = comp->knee_db * 0.5f;
	float dist = level_dbr - comp->target_dbr;

	// Required gain in dB (power ratio -> amplitude dB = 0.5 * 10*log10)
	float gain_db = comp->strenght * (comp->target_dbr - level_dbr) * 0.5f;

	// Limit to the gain range actually allowed
	// 20.0f * log10f(0.01f) = -40.0f
	gain_db = CLAMP(gain_db, -40.0f, 20.0f * log10f(fmaxf(comp->max_gain, 0.01f)));

	float knee_blend;
	if (dist <= -half_knee) knee_blend = 0.0f;
	else if (dist >= half_knee) knee_blend = 1.0f;
	else {
		float t = (dist + half_knee) / comp->knee_db;
		knee_blend = t * t * (3.0f - 2.0f * t);
	}

	// Blend in the dB domain: always finite
	float blended_target = powf(10.0f, (knee_blend * gain_db) / 20.0f);

	float coeff = (comp->avg_power > comp->target) ? comp->attack : comp->release;
	comp->gain = coeff * comp->gain + (1.0f - coeff) * blended_target;
	if (!isfinite(comp->gain)) comp->gain = 1.0f;
	comp->gain = CLAMP(comp->gain, 0.01f, comp->max_gain);
	if (!isfinite(comp->avg_power)) comp->avg_power = 0.0;

	*mpx_power = level_dbr;

	return output_sample;
}