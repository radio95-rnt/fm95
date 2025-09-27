#include <getopt.h>
#include <stdio.h>

#define buffer_maxlength 12288
#define buffer_tlength_fragsize 12288
#define buffer_prebuf 8

#define DEFAULT_FREQUENCY 67000.0f
#define DEFAULT_DEVIATION 7000.0f
#define DEFAULT_CLIPPER_THRESHOLD 1.0f

#include "../modulation/fm_modulator.h"

#define DEFAULT_SAMPLE_RATE 192000

#define INPUT_DEVICE "SCA.monitor"
#define OUTPUT_DEVICE "FM_MPX"

#define BUFFER_SIZE 2048

#include "../io/audio.h"

#define DEFAULT_AUDIO_VOLUME 1.0f // Audio volume, before clipper

#define DEFAULT_VOLUME 0.1f

static volatile sig_atomic_t to_run = 1;

inline float hard_clip(float sample, float threshold) { return fmaxf(-threshold, fminf(threshold, sample)); }

typedef struct {
	float freq;
	float deviation;
	float clipper;
	float volume;
	float master_volume;
	float audio_volume;
	uint32_t sample_rate;
} Sca95_Config;
typedef struct
{
	PulseInputDevice input;
	PulseOutputDevice output;
} Sca95_Runtime;

static void stop(int signum) {
	(void)signum;
	printf("\nReceived stop signal.\n");
	to_run = 0;
}

void show_help(char *name) {
	printf(
		"Usage: \t%s\n"
		"\t-i,--input\tOverride input device [default: %s]\n"
		"\t-o,--output\tOverride output device [default: %s]\n"
		"\t-f,--sca_freq\tOverride the SCA frequency [default: %.1f]\n"
		"\t-F,--sca_dev\tOverride the SCA deviation [default: %.2f]\n"
		"\t-C,--sca_clip\tOverride the SCA clipper threshold [default: %.2f]\n"
		"\t-A,--master_vol\tSet master volume [default: %.3f]\n"
		"\t-v,--volume\tSet audio volume [default: %.3f]\n"
		,name
		,INPUT_DEVICE
		,OUTPUT_DEVICE
		,DEFAULT_FREQUENCY
		,DEFAULT_DEVIATION
		,DEFAULT_CLIPPER_THRESHOLD
		,DEFAULT_VOLUME
		,DEFAULT_AUDIO_VOLUME
	);
}

int run_sca95(const Sca95_Config config, Sca95_Runtime* runtime) {
	FMModulator sca_mod;
	init_fm_modulator(&sca_mod, config.freq, config.deviation, config.sample_rate);

	int pulse_error;

	float audio_input[BUFFER_SIZE];
	float output[BUFFER_SIZE];

	while (to_run) {
		if((pulse_error = read_PulseInputDevice(&runtime->input, audio_input, sizeof(audio_input)))) {
			fprintf(stderr, "Error reading from input device: %s\n", pa_strerror(pulse_error));
			to_run = 0;
			break;
		}

		for (uint16_t i = 0; i < BUFFER_SIZE; i++) {
			output[i] = modulate_fm(&sca_mod, hard_clip(audio_input[i]*config.audio_volume, config.clipper))*config.master_volume;
		}

		if((pulse_error = write_PulseOutputDevice(&runtime->output, output, sizeof(output)))) {
			fprintf(stderr, "Error writing to output device: %s\n", pa_strerror(pulse_error));
			to_run = 0;
			break;
		}
	}
	return 0;
}

int main(int argc, char **argv) {
	printf("sca95 (a SCA modulator by radio95) version 1.1\n");

	Sca95_Config config = {
		.freq = DEFAULT_FREQUENCY,
		.deviation = DEFAULT_DEVIATION,
		.clipper = DEFAULT_CLIPPER_THRESHOLD,
		.master_volume = DEFAULT_VOLUME,
		.audio_volume = DEFAULT_AUDIO_VOLUME,
		.sample_rate = DEFAULT_SAMPLE_RATE
	};

	char audio_input_device[64] = INPUT_DEVICE;
	char audio_output_device[64] = OUTPUT_DEVICE;

	int opt;
	const char	*short_opt = "i:o:f:F:C:A:v:h";
	struct option	long_opt[] =
	{
		{"input",       required_argument, NULL, 'i'},
		{"output",      required_argument, NULL, 'o'},
		{"sca_freq",    required_argument, NULL, 'f'},
		{"sca_dev",     required_argument, NULL, 'F'},
		{"sca_clip",    required_argument, NULL, 'C'},
		{"master_vol",     required_argument,       NULL, 'A'},
		{"output",     required_argument,       NULL, 'A'},
		{"audio_vol",     required_argument,       NULL, 'v'},

		{"help",        no_argument,       NULL, 'h'},
		{0,             0,                 0,    0}
	};

	while((opt = getopt_long(argc, argv, short_opt, long_opt, NULL)) != -1) {
		switch(opt) {
			case 'i': // Input Device
				memcpy(audio_input_device, optarg, 47);
				break;
			case 'o': // Output Device
				memcpy(audio_output_device, optarg, 47);
				break;
			case 'f': //SCA freq
				config.freq = strtof(optarg, NULL);
				break;
			case 'F': //SCA deviation
				config.deviation = strtof(optarg, NULL);
				break;
			case 'C': //SCA clip
				config.clipper = strtof(optarg, NULL);
				break;
			case 'A': // Master vol
				config.master_volume = strtof(optarg, NULL);
				break;
			case 'v': // Audio Volume
				config.audio_volume = strtof(optarg, NULL);
				break;
			case 'h':
				show_help(argv[0]);
				return 1;
		}
	}

	pa_buffer_attr input_buffer_atr = {
		.maxlength = buffer_maxlength,
		.fragsize = buffer_tlength_fragsize
	};
	pa_buffer_attr output_buffer_atr = {
		.maxlength = buffer_maxlength,
		.tlength = buffer_tlength_fragsize,
		.prebuf = buffer_prebuf
	};

	int opentime_pulse_error;

	Sca95_Runtime runtime;
	memset(&runtime, 0, sizeof(runtime));

	printf("Connecting to input device... (%s)\n", audio_input_device);
	opentime_pulse_error = init_PulseInputDevice(&runtime.input, config.sample_rate, 1, "sca95", "Main Audio Input", audio_input_device, &input_buffer_atr, PA_SAMPLE_FLOAT32NE);
	if (opentime_pulse_error) {
		fprintf(stderr, "Error: cannot open input device: %s\n", pa_strerror(opentime_pulse_error));
		return 1;
	}

	printf("Connecting to output device... (%s)\n", audio_output_device);

	opentime_pulse_error = init_PulseOutputDevice(&runtime.output, config.sample_rate, 1, "sca95", "Signal Output", audio_output_device, &output_buffer_atr, PA_SAMPLE_FLOAT32NE);
	if (opentime_pulse_error) {
		fprintf(stderr, "Error: cannot open output device: %s\n", pa_strerror(opentime_pulse_error));
		free_PulseDevice(&runtime.input);
		return 1;
	}

	signal(SIGINT, stop);
	signal(SIGTERM, stop);

	int ret = run_sca95(config, &runtime);
	printf("Cleaning up...\n");
	free_PulseDevice(&runtime.input);
	free_PulseDevice(&runtime.output);
	return ret;
}