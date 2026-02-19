#include "bs412.h"

#define BS412_TIME 60
#define CLAMP(x, lo, hi) (((x) < (lo)) ? (lo) : ((x) > (hi) ? (hi) : (x)))

#define SQRT19000 180499999.99999997f // (19000 / sqrt(2)) * 19000 / sqrt(2)

// inline float dbr_to_deviation(float dbr) {
// 	return 19000.0f * sqrtf(pow(10.0, dbr / 10.0));
// }

inline float deviation_to_dbr(float deviation) {
	if (deviation < 1e-6f) return -100.0f;
	return 10*log10f((deviation*deviation)/SQRT19000);
}

void init_bs412(BS412Compressor* comp, uint32_t mpx_deviation, float target_power, float attack, float release, float max_gain, uint32_t sample_rate) {
	comp->mpx_deviation = mpx_deviation;
	comp->avg_power = 0.0f;
    comp->alpha = 1.0f / (BS412_TIME * sample_rate);
	comp->sample_rate = sample_rate;
	comp->attack = expf(-1.0f / (attack * sample_rate));
	comp->release = expf(-1.0f / (release * sample_rate));
	comp->target = deviation_to_dbr(19000.0f * pow(10.0, target_power / 10.0)); // target is expected to not be our rms format
	comp->gain = 0.0f;
	comp->can_compress = 0;
	comp->second_counter = 0;
	comp->last_output = 0.0f;
	comp->max_gain = max_gain;
	#ifdef BS412_DEBUG
	debug_printf("Initialized MPX power measurement with sample rate: %d\n", sample_rate);
	#endif
}

float bs412_compress(BS412Compressor* comp, float audio, float sample_mpx) {
	float combined = audio + sample_mpx;

	comp->avg_power += comp->alpha * ((comp->last_output * comp->last_output * comp->mpx_deviation * comp->mpx_deviation) - comp->avg_power);

	float avg_deviation = sqrtf(comp->avg_power);
	float modulation_power = deviation_to_dbr(avg_deviation);

	if(comp->target <= -100.0f) {
		#ifdef BS412_DEBUG
		if(comp->sample_counter > comp->sample_rate) {
			debug_printf("MPX power: %.2f dBr (%.1f Hz)\n", modulation_power, avg_deviation);
			comp->sample_counter = 0;
		}
		#endif
		comp->sample_counter++;
		return combined;
	}

	if(comp->sample_counter > comp->sample_rate) {
		#ifdef BS412_DEBUG
		debug_printf("MPX power: %.2f dBr with gain %.2fx (%.2f dBr)\n", modulation_power, mpx->gain, deviation_to_dbr(avg_deviation * mpx->gain));
		#endif
		comp->sample_counter = 0;
		if(comp->can_compress == 0) comp->second_counter++;
	}

	if(comp->can_compress == 0 && comp->second_counter > BS412_TIME) {
		#ifdef BS412_DEBUG
		debug_printf("Can compress.\n");
		#endif
		comp->gain = powf(10.0f, (comp->target - modulation_power) / 10.0f);
		comp->can_compress = 1;
	}

	if(comp->can_compress == 0) {
		comp->sample_counter++;
		return combined;
	}

	float target_gain = expf((comp->target - modulation_power) * 0.2302585093f); // 1/10 * ln(10)
	if (modulation_power > comp->target) comp->gain = comp->attack * comp->gain + (1.0f - comp->attack) * target_gain;
	else comp->gain = comp->release * comp->gain + (1.0f - comp->release) * target_gain;
	
	comp->gain = CLAMP(comp->gain, 0.0f, comp->max_gain);

	float output_sample = (audio * comp->gain) + sample_mpx;
	if(deviation_to_dbr(avg_deviation * comp->gain) > comp->target && modulation_power < comp->target) {
		// Gain is too much, reduce
		float overshoot_dbr = deviation_to_dbr(avg_deviation * comp->gain) - comp->target;
		float reduction_factor = powf(10.0f, -overshoot_dbr / 10.0f);
		comp->gain *= reduction_factor;
		comp->gain = fmaxf(0.01f, fminf(comp->max_gain, comp->gain));
	}

	comp->sample_counter++;

	comp->last_output = output_sample;
	return output_sample;
}
