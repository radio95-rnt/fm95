#include "bs412.h"

inline float dbr_to_deviation(float dbr) {
	return 19000.0f * pow(10.0, dbr / 10.0);
}

inline float deviation_to_dbr(float deviation) {
	if (deviation < 1e-6f) return -100.0f;
	return 10*log10f((deviation*deviation)/((19000.0f / sqrtf(2.0f)) * (19000.0f / sqrtf(2.0f))));
}

void init_bs412(BS412Compressor* mpx, uint32_t mpx_deviation, float target_power, float attack, float release, float max, uint32_t sample_rate) {
	mpx->mpx_deviation = mpx_deviation;
	mpx->avg_power = 0.0f;
    mpx->alpha = 1.0f / (60.0f * sample_rate);
	mpx->sample_rate = sample_rate;
	mpx->attack = expf(-1.0f / (attack * sample_rate));
	mpx->release = expf(-1.0f / (release * sample_rate));
	mpx->target = target_power;
	mpx->gain = 0.0f;
	mpx->max = max;
	mpx->can_compress = 0;
	mpx->second_counter = 0;
	#ifdef BS412_DEBUG
	debug_printf("Initialized MPX power measurement with sample rate: %d\n", sample_rate);
	#endif
}

static inline float soft_clip_tanh(float sample, float threshold) {
    if (fabsf(sample) <= threshold) {
        return sample;  // Linear region
    }
    float sign = (sample >= 0) ? 1.0f : -1.0f;
    float excess = fabsf(sample) - threshold;
    return sign * (threshold + tanhf(excess) * (1.0f - threshold));
}

float bs412_compress(BS412Compressor* mpx, float sample) {
	mpx->avg_power += mpx->alpha * ((sample * sample * mpx->mpx_deviation * mpx->mpx_deviation) - mpx->avg_power);

	float avg_deviation = sqrtf(mpx->avg_power);
	float modulation_power = deviation_to_dbr(avg_deviation);

	if(mpx->target <= -100.0f) {
		#ifdef BS412_DEBUG
		if(mpx->sample_counter > mpx->sample_rate) {
			debug_printf("MPX power: %.2f dBr (%.1f Hz)\n", modulation_power, avg_deviation);
			mpx->sample_counter = 0;
		}
		#endif
		mpx->sample_counter++;
		return sample;
	}

	if(mpx->sample_counter > mpx->sample_rate) {
		#ifdef BS412_DEBUG
		debug_printf("MPX power: %.2f dBr with gain %.2fx (%.2f dBr)\n", modulation_power, mpx->gain, deviation_to_dbr(avg_deviation * mpx->gain));
		#endif
		mpx->sample_counter = 0;
		if(mpx->can_compress == 0) mpx->second_counter++;
	}

	if(mpx->can_compress == 0 && mpx->second_counter > 60) {
		#ifdef BS412_DEBUG
		debug_printf("Can compress.\n");
		#endif
		mpx->gain = powf(10.0f, (mpx->target - modulation_power) / 10.0f);
		mpx->can_compress = 1;
	}

	if(mpx->can_compress == 0) {
		mpx->sample_counter++;
		return sample;
	}

	float target_gain = powf(10.0f, (mpx->target - modulation_power) / 10.0f);
	if (modulation_power > mpx->target) {
		mpx->gain = mpx->attack * mpx->gain + (1.0f - mpx->attack) * target_gain;
	} else {
		mpx->gain = mpx->release * mpx->gain + (1.0f - mpx->release) * target_gain;
	}
	
	mpx->gain = fmaxf(0.0f, fminf(mpx->max, mpx->gain));

	float output_sample = sample * mpx->gain;
	float limit_threshold = dbr_to_deviation(mpx->target + 0.1f) / mpx->mpx_deviation;
    output_sample = soft_clip_tanh(output_sample, limit_threshold);

	if(deviation_to_dbr(avg_deviation * mpx->gain) > mpx->target && deviation_to_dbr(avg_deviation) < mpx->target) {
		// Gain is too much, reduce
		float overshoot_dbr = deviation_to_dbr(avg_deviation * mpx->gain) - mpx->target;
		float reduction_factor = powf(10.0f, -overshoot_dbr / 10.0f);
		mpx->gain *= reduction_factor;
		mpx->gain = fmaxf(0.0f, fminf(mpx->max, mpx->gain));
	}

	mpx->sample_counter++;
	return output_sample;
}
