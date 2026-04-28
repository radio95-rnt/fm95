#include "bs412.h"

#define BS412_TIME 60
#define ENABLE_TIME 50
#define CLAMP(x, lo, hi) (((x) < (lo)) ? (lo) : ((x) > (hi) ? (hi) : (x)))

inline float dbr_to_deviation(float dbr) {
	return 19000.0f * sqrtf(pow(10.0, dbr / 10.0));
}

// #define SQRT19000 180499999.99999997f // (19000 / sqrt(2)) * 19000 / sqrt(2)
// inline float deviation_to_dbr(float deviation) {
// 	if (deviation < 1e-6f) return -100.0f;
// 	return 10*log10f((deviation*deviation)/SQRT19000);
// }

void init_bs412(BS412Compressor* comp, uint32_t mpx_deviation, float target_power, float attack, float release, float max_gain, uint32_t sample_rate) {
	comp->mpx_deviation = mpx_deviation;
	comp->avg_power = 0.0f;
    comp->alpha = 1.0f / (BS412_TIME * sample_rate);
	comp->sample_rate = sample_rate;
	comp->attack = expf(-1.0f / (attack * sample_rate));
	comp->release = expf(-1.0f / (release * sample_rate));
	comp->target = dbr_to_deviation(target_power) * dbr_to_deviation(target_power);
	comp->gain = 1.0f;
	comp->can_compress = 0;
	comp->second_counter = 0;
	comp->max_gain = max_gain;
	#ifdef BS412_DEBUG
	debug_printf("Initialized MPX power measurement with sample rate: %d\n", sample_rate);
	#endif
}

float bs412_compress(BS412Compressor* comp, float audio, float sample_mpx) {
	float combined = audio + sample_mpx;
	float output_sample = (audio * comp->gain) + sample_mpx;

	comp->avg_power += comp->alpha * ((output_sample * output_sample * comp->mpx_deviation * comp->mpx_deviation) - comp->avg_power);

	if(comp->sample_counter > comp->sample_rate) {
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
	if (comp->avg_power > comp->target) comp->gain = comp->attack * comp->gain + (1.0f - comp->attack) * target_gain;
	else comp->gain = comp->release * comp->gain + (1.0f - comp->release) * target_gain;
	
	comp->gain = CLAMP(comp->gain, 0.0f, comp->max_gain);

	float power_after_gain = comp->avg_power * comp->gain * comp->gain;

	if(power_after_gain > comp->target && comp->avg_power < comp->target) {
		float reduction = sqrtf(comp->target / power_after_gain);
		comp->gain = CLAMP(comp->gain * reduction, 0.01f, comp->max_gain);
	}

	comp->sample_counter++;

	if(comp->can_compress) return output_sample;
	return combined;
}
