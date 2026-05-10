#include "bs412.h"

#define BS412_TIME 60
#define ENABLE_TIME 45
#define CLAMP(x, lo, hi) (((x) < (lo)) ? (lo) : ((x) > (hi) ? (hi) : (x)))

inline float power_to_dbr(float power, float ref) {
    if (power < 1e-12f) return -100.0f;
    return 10*log10f(power / ref);
}

void init_bs412(BS412Compressor* comp, uint32_t mpx_deviation, float target_power, float attack, float release, float max_gain, float gate, float knee_db, uint32_t sample_rate) {
	comp->reference = (19000.0f / mpx_deviation) * (19000.0f / mpx_deviation);
	comp->avg_power = 0.0f;
    comp->alpha = 1.0f / (BS412_TIME * sample_rate);
	comp->sample_rate = sample_rate;
	comp->attack = expf(-1.0f / (attack * sample_rate));
	comp->release = expf(-1.0f / (release * sample_rate));
	comp->target = comp->reference * powf(10.0f, target_power / 10.0f);
	comp->target_dbr = power_to_dbr(comp->target, comp->reference);
	comp->gain = 1.0f;
	comp->can_compress = 0;
	comp->second_counter = 0;
	comp->max_gain = max_gain;
	comp->gate_threshold = comp->reference * powf(10.0f, gate / 10.0f);
	comp->knee_db = knee_db; // e.g. 6.0f — width in dB around target on each side
	comp->init = true;
	#ifdef BS412_DEBUG
	debug_printf("Initialized MPX power measurement with sample rate: %d\n", sample_rate);
	#endif
}

void reinit_bs412(BS412Compressor* comp, uint32_t mpx_deviation, float target_power, float attack, float release, float max_gain, float gate, float knee_db) {
	comp->reference = (19000.0f / mpx_deviation) * (19000.0f / mpx_deviation);
	comp->target = comp->reference * powf(10.0f, target_power / 10.0f);
	comp->target_dbr = power_to_dbr(comp->target, comp->reference);
	comp->gate_threshold = comp->reference * powf(10.0f, gate / 10.0f);
	comp->attack = expf(-1.0f / (attack * comp->sample_rate));
	comp->release = expf(-1.0f / (release * comp->sample_rate));
	comp->max_gain = max_gain;
	comp->knee_db = knee_db;
}

float bs412_compress(BS412Compressor* comp, float audio, float sample_mpx, float* mpx_power) {
	float combined = audio + sample_mpx;
	float output_sample = (audio * comp->gain) + sample_mpx;

	float inst_power = output_sample * output_sample;
	float audio_power = (audio * comp->gain) * (audio * comp->gain);

	float w = (audio_power - comp->gate_threshold) / comp->gate_threshold;
	w = CLAMP(w, 0.0f, 1.0f);
	comp->avg_power += comp->alpha * w * (inst_power - comp->avg_power);

	if(comp->sample_counter >= comp->sample_rate) {
		comp->sample_counter = 0;
		if(comp->can_compress == 0) comp->second_counter++;
	}

	if(comp->can_compress == 0 && comp->second_counter > ENABLE_TIME) {
		#ifdef BS412_DEBUG
		debug_printf("Can compress.\n");
		#endif
		comp->can_compress = 1;
		comp->second_counter = 0;
	}

	float safe_power = fmaxf(comp->avg_power, 1e-12f);
	float target_gain = sqrtf(comp->target / safe_power);

	float level_dbr = power_to_dbr(comp->avg_power, comp->reference);
	float half_knee = comp->knee_db * 0.5f;
	float dist = level_dbr - comp->target_dbr; // negative = below target, positive = above

	float knee_blend;
	if (dist <= -half_knee) knee_blend = 0.0f; // well below target — don't pull gain toward target
	else if (dist >= half_knee) knee_blend = 1.0f; // well above target — full compression
	else {
		// Smooth cubic ramp: 0→1 over the knee width
		float t = (dist + half_knee) / comp->knee_db; // 0..1
		knee_blend = t * t * (3.0f - 2.0f * t);       // smoothstep
	}

	float blended_target = 1.0f + knee_blend * (target_gain - 1.0f);

	float coeff = (comp->avg_power > comp->target) ? comp->attack : comp->release;
	comp->gain = coeff * comp->gain + (1.0f - coeff) * blended_target;
	comp->gain = CLAMP(comp->gain, 0.01f, comp->max_gain);

	comp->sample_counter++;

	if(mpx_power != NULL) *mpx_power = level_dbr;

	if(comp->can_compress) return output_sample;
	return combined;
}