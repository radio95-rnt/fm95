#include "bs412.h"

#define BS412_TIME 60
#define ENABLE_TIME 45
#define CLAMP(x, lo, hi) (((x) < (lo)) ? (lo) : ((x) > (hi) ? (hi) : (x)))

inline float power_to_dbr(float power, float ref) {
    if (power < 1e-12f) return -100.0f;
    return 10*log10f(power / ref);
}

void init_bs412(BS412Compressor* comp, uint32_t mpx_deviation, float target_power, float attack, float release, float max_gain, float gate, uint32_t sample_rate) {
	comp->reference = (19000.0f / mpx_deviation) * (19000.0f / mpx_deviation); // 0 dbr is a signal which generates a deviation of 19 khz
	comp->avg_power = 0.0f;
    comp->alpha = 1.0f / (BS412_TIME * sample_rate);
	comp->sample_rate = sample_rate;
	comp->attack = expf(-1.0f / (attack * sample_rate));
	comp->release = expf(-1.0f / (release * sample_rate));
	comp->target = comp->reference * powf(10.0f, target_power / 10.0f);;
	comp->gain = 1.0f;
	comp->can_compress = 0;
	comp->second_counter = 0;
	comp->max_gain = max_gain;
	comp->gate_threshold = comp->reference * powf(10.0f, gate / 10.0f);
	comp->init = true;
	#ifdef BS412_DEBUG
	debug_printf("Initialized MPX power measurement with sample rate: %d\n", sample_rate);
	#endif
}

void reinit_bs412(BS412Compressor* comp, uint32_t mpx_deviation, float target_power, float attack, float release, float max_gain, float gate) {
	comp->reference = (19000.0f / mpx_deviation) * (19000.0f / mpx_deviation); // 0 dbr is a signal which generates a deviation of 19 khz
	comp->target = comp->reference * powf(10.0f, target_power / 10.0f);;
	comp->gate_threshold = comp->reference * powf(10.0f, gate / 10.0f);
	comp->attack = expf(-1.0f / (attack * sample_rate));
	comp->release = expf(-1.0f / (release * sample_rate));
	comp->max_gain = max_gain;
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
	if (comp->avg_power > comp->target)
		comp->gain = comp->attack * comp->gain + (1.0f - comp->attack) * target_gain;
	else
		comp->gain = comp->release * comp->gain + (1.0f - comp->release) * target_gain;
	
	comp->gain = CLAMP(comp->gain, 0.01f, comp->max_gain);

	comp->sample_counter++;

	if(mpx_power != NULL) *mpx_power = power_to_dbr(comp->avg_power, comp->reference);

	if(comp->can_compress) return output_sample;
	return combined;
}
