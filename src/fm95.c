#include <getopt.h>
#include <liquid/liquid.h>
#include "ini.h"
#include <stdbool.h>

#define DEFAULT_INI_PATH "/etc/fm95/fm95.conf"

#define buffer_maxlength 99960
#define buffer_tlength_fragsize 99960

#include "oscillator.h"
#include "iir.h"
#include "stereo_encoder.h"
#include "bs412.h"
#include "gain_control.h"
#include "bit_ring.h"

#define BUFFER_SIZE 4998 // This defines how many samples to process at a time, because the loop here is this: get signal -> process signal -> output signal, and when we get signal we actually get BUFFER_SIZE of them

#include "audio.h"
#include "ipc.h"

static volatile sig_atomic_t to_run = 1;
static volatile sig_atomic_t to_reload = 0;

typedef struct {
	bool mpx_on;
} FM95_Options;
typedef struct {
	float audio;
	float headroom;
	float pilot;
	float rds;
	float rds_step;
	float drive;
	float makeup;
} FM95_Volumes;
typedef struct {
	FM95_Options options;

	FM95_Volumes volumes;
	bool stereo;
	int stereo_ssb;

	uint8_t rds_streams;

	uint8_t preemphasis;
	uint8_t calibration;
	float mpx_power;
	float mpx_deviation;
	float audio_deviation;
	float master_volume;
	float audio_preamp;

	uint32_t sample_rate;

	char ini_config_path[64];

	uint8_t lpf_order;
	float preemp_unity_freq;
	float agc_target;
	float agc_attack;
	float agc_release;
	float agc_max;
	float agc_min;
	float bs412_attack;
	float bs412_release;
	float bs412_max;
	float bs412_gate;
	float bs412_knee;
	float bs412_strenght;
	float lpf_cutoff;
} FM95_Config;

typedef struct {
	PulseInputDevice input_device, mpx_device;
	PulseOutputDevice output_device;
	Oscillator osc;
	iirfilt_rrrf lpf_l, lpf_r;
	ResistorCapacitor preemp_l, preemp_r;
	BS412Compressor bs412;
	StereoEncoder stencode;
	AGC agc;
	delay_line_t rds_delays[4];
	bit_ring_t rds_bitring[4];
	float rds_symbol[4];
	uint8_t rds_last_bit[4];
	iirfilt_rrrf rds_filter[4];
} FM95_Runtime;

typedef struct {
    char input[64];
    char output[64];
    char mpx[64];
} FM95_DeviceNames;
typedef struct {
    FM95_Config* config;
    FM95_DeviceNames* devices;
} FM95_SetupContext;

typedef struct {
	float mpx_power;
	float bs412_gain;
	float agc_gain;
	float input_level;
	float audio_level;
} FM95_RunResult;

static inline bool compare_dvs(const FM95_DeviceNames *a, const FM95_DeviceNames *b) {
    return strcmp(a->input, b->input) == 0 && strcmp(a->output, b->output) == 0 && strcmp(a->mpx, b->mpx) == 0;
}

static float calculate_sharedaudio_volume(const FM95_Volumes volumes, const int rds_streams) {
	float rds_volume = 0.0f;
	for (int i = 0; i < rds_streams; i++) rds_volume += volumes.rds * powf(volumes.rds_step, i);
	return 1.0f - rds_volume - volumes.pilot - volumes.headroom;
}

static void stop(int signum) {
	(void)signum;
	printf("\nReceived stop signal.\n");
	to_run = 0;
	to_reload = 0; // Make sure we don't reload
}
static void reload(int signum) {
	(void)signum;
	printf("\nReceived reload signal.\n");
	to_run = 0; // To run is a flag, just telling when to stop the loop
	to_reload = 1;
}

void show_help(char *name) {
	printf(
		"Usage: \t%s\n"
		"\t-c,--config\tOverride the default config path (%s)\n",
		name,
		DEFAULT_INI_PATH
	);
}

void cleanup_runtime(FM95_Runtime* runtime, const FM95_Config config) {
	if(config.lpf_cutoff != 0) {
		if(runtime->lpf_l != NULL) iirfilt_rrrf_destroy(runtime->lpf_l);
		if(runtime->lpf_r != NULL) iirfilt_rrrf_destroy(runtime->lpf_r);
		runtime->lpf_l = runtime->lpf_r = NULL;
	} exit_stereo_encoder(&runtime->stencode);

	for(int i = 0; i < 4; i++) free(runtime->rds_bitring[i].bits);
	for(int i = 0; i < 4; i++) iirfilt_rrrf_destroy(runtime->rds_filter[i]);
}

void cleanup_audio_runtime(FM95_Runtime *rt, const FM95_Options options) {
    free_PulseDevice(&rt->input_device);
    if (options.mpx_on) free_PulseDevice(&rt->mpx_device);
    free_PulseDevice(&rt->output_device);
}

#define _pulse_output \
if((pulse_error = write_PulseOutputDevice(&runtime->output_device, output, sizeof(output)))) { \
	fprintf(stderr, "Error writing to output device: %s\n", pa_strerror(pulse_error)); \
	to_run = 0; \
	break; \
}

int run_fm95(FM95_Config* config, FM95_Runtime* runtime, FM95_RunResult* result) {
	float output[BUFFER_SIZE];
	FM95_RunResult temp_result;
	memset(&temp_result, 0, sizeof(FM95_RunResult));

	int pulse_error;

	if(config->calibration != 0) {
		while(to_run) {
			for (int i = 0; i < BUFFER_SIZE; i++) {
				float sample = get_oscillator_sin_sample(&runtime->osc);
				if(config->calibration == 2) sample = (sample > 0.0f) ? 1.0f : -1.0f; // Sine wave to square wave filter, 50% duty cycle
				else if(config->calibration == 3) sample *= (19000/config->mpx_deviation);
				output[i] = sample*config->master_volume;
			} _pulse_output;
		}
		return 0;
	}

	float audio_stereo_input[BUFFER_SIZE*2]; // Stereo

	float mpx_in[BUFFER_SIZE] = {0};

	bool mpx_on = config->options.mpx_on;

	while (to_run) {
		float softclip_norm = config->volumes.makeup / tanhf(config->volumes.drive);

		if((pulse_error = read_PulseInputDevice(&runtime->input_device, audio_stereo_input, sizeof(audio_stereo_input)))) { // get output from the function and assign it into pulse_error, this comment to avoid confusion
			fprintf(stderr, "Error reading from input device: %s\n", pa_strerror(pulse_error));
			to_run = 0;
			break;
		}
		if(mpx_on) {
			if((pulse_error = read_PulseInputDevice(&runtime->mpx_device, mpx_in, sizeof(mpx_in)))) {
				fprintf(stderr, "Error reading from MPX device: %s\nDisabling MPX.\n", pa_strerror(pulse_error));
				mpx_on = 0;
			}
		}

		for(uint16_t i = 0; i < BUFFER_SIZE; i++) {
			bool cycle = advance_oscillator(&runtime->osc);
			bool do_result = (i == BUFFER_SIZE - 1);

			float mpx = 0.0f;
			float audio = 0.0f;

			float l = audio_stereo_input[2*i+0]*config->audio_preamp;
			float r = audio_stereo_input[2*i+1]*config->audio_preamp;

			float mono = 0.5f * (fabsf(l) + fabsf(r));
			temp_result.input_level = mono;

			if(config->agc_max != 0.0) {
				float agc_gain = process_agc(&runtime->agc, mono);
				l *= agc_gain;
				r *= agc_gain;
				temp_result.agc_gain = agc_gain;
			} else temp_result.agc_gain = 0.0f;

			float mod_l, mod_r;

			if(config->lpf_cutoff != 0) {
				iirfilt_rrrf_execute(runtime->lpf_l, l, &mod_l);
				iirfilt_rrrf_execute(runtime->lpf_r, r, &mod_r);
			}
	
			if(config->preemphasis != 0) {
				mod_l = apply_preemphasis(&runtime->preemp_l, mod_l);
				mod_r = apply_preemphasis(&runtime->preemp_r, mod_r);
			}

			mod_l = tanhf(mod_l * config->volumes.drive) * softclip_norm;
			mod_r = tanhf(mod_r * config->volumes.drive) * softclip_norm;

			if(do_result) temp_result.audio_level = (mod_l + mod_r) * 0.5f;

			mpx = stereo_encode(&runtime->stencode, config->stereo, mod_l, mod_r, &audio);

			{
				float rds_level = config->volumes.rds;
				float clock = get_oscillator_cos_multiplier_ni(&runtime->osc, 1.0f);
				for(uint8_t stream = 0; stream < config->rds_streams; stream++) {
					if (cycle) {
						uint8_t bit;
						if (bit_ring_read1(&runtime->rds_bitring[stream], &bit)) {
							runtime->rds_last_bit[stream] = bit;
						}
						runtime->rds_symbol[stream] = runtime->rds_last_bit[stream] ? 1.0f : -1.0f;
					}

					uint8_t osc_stream = 12 + stream;
					if(osc_stream >= 13) osc_stream++;
					
					float shaped = 0.0f;
					iirfilt_rrrf_execute(runtime->rds_filter[stream], runtime->rds_symbol[stream] * clock, &shaped);

					float carrier = get_oscillator_cos_multiplier_ni(&runtime->osc, osc_stream * 4.0f);
					if(config->stereo_ssb) carrier = delay_line(&runtime->rds_delays[stream], carrier);
					mpx += shaped * carrier * rds_level;
					rds_level *= config->volumes.rds_step; // Prepare level for the next stream
				}
			}

			mpx = bs412_compress(&runtime->bs412, audio, mpx+mpx_in[i], &temp_result.mpx_power);
			temp_result.bs412_gain = runtime->bs412.gain;

			output[i] = tanhf(mpx)*config->master_volume; // Ensure peak deviation of 75 khz (or the set deviation), assuming we're calibrated correctly
		} memcpy(result, &temp_result, sizeof(FM95_RunResult));
		_pulse_output;
	}

	return 0;
}

int parse_arguments(int argc, char **argv, FM95_Config* config) {
	int opt;
	const char *short_opt = "c:h";
	struct option long_opt[] = {
		{"config", required_argument, NULL,	'c'},
		{"help", no_argument, NULL, 'h'},
		{0, 0, 0, 0}
	};

	while((opt = getopt_long(argc, argv, short_opt, long_opt, NULL)) != -1) {
		switch(opt) {
			case 'c':
				memcpy(config->ini_config_path, optarg, 63);
				break;
			case 'h':
				show_help(argv[0]);
				return 1;
		}
	}

	return 0;
}

static int config_handler(void* user, const char* section, const char* name, const char* value) {
    FM95_SetupContext* ctx = (FM95_SetupContext*)user;
    FM95_Config* pconfig = ctx->config;
    FM95_DeviceNames* dv = ctx->devices;

    #define MATCH(s, n) strcmp(section, s) == 0 && strcmp(name, n) == 0

    if (MATCH("fm95", "stereo")) pconfig->stereo = atoi(value);
	else if (MATCH("devices", "input")) {
        strncpy(dv->input, value, 63);
        dv->input[63] = '\0';
    } else if (MATCH("devices", "output")) {
        strncpy(dv->output, value, 63);
        dv->output[63] = '\0';
    } else if (MATCH("devices", "mpx")) {
        strncpy(dv->mpx, value, 63);
        dv->mpx[63] = '\0';
    } else if (MATCH("fm95", "rds_streams")) {
        pconfig->rds_streams = atoi(value);
        if(pconfig->rds_streams > 4) {
            printf("RDS Streams more than 4? Nuh uh\n");
            return 0;
        }
	} else if (MATCH("fm95", "preemphasis")) pconfig->preemphasis = atoi(value);
    else if (MATCH("fm95", "calibration")) pconfig->calibration = atoi(value);
    else if (MATCH("fm95", "mpx_power")) pconfig->mpx_power = strtof(value, NULL);
    else if (MATCH("fm95", "mpx_deviation")) pconfig->mpx_deviation = strtof(value, NULL);
    else if (MATCH("fm95", "master_volume")) pconfig->master_volume = strtof(value, NULL);
    else if (MATCH("fm95", "audio_preamp")) pconfig->audio_preamp = strtof(value, NULL);
    else if (MATCH("fm95", "deviation")) pconfig->audio_deviation = strtof(value, NULL);
	else if(MATCH("fm95", "agc_target")) pconfig->agc_target = strtof(value, NULL);
	else if(MATCH("fm95", "agc_attack")) pconfig->agc_attack = strtof(value, NULL);
	else if(MATCH("fm95", "agc_release")) pconfig->agc_release = strtof(value, NULL);
	else if(MATCH("fm95", "agc_min")) pconfig->agc_min = strtof(value, NULL);
	else if(MATCH("fm95", "agc_max")) pconfig->agc_max = strtof(value, NULL);
	else if(MATCH("fm95", "bs412_attack")) pconfig->bs412_attack = strtof(value, NULL);
	else if(MATCH("fm95", "bs412_release")) pconfig->bs412_release = strtof(value, NULL);
	else if(MATCH("fm95", "bs412_max")) pconfig->bs412_max = strtof(value, NULL);
	else if(MATCH("fm95", "bs412_gate")) pconfig->bs412_gate = strtof(value, NULL);
	else if(MATCH("fm95", "bs412_knee")) pconfig->bs412_knee = strtof(value, NULL);
	else if(MATCH("fm95", "bs412_strenght")) pconfig->bs412_strenght = strtof(value, NULL);
	else if(MATCH("advanced", "lpf_order")) pconfig->lpf_order = atoi(value);
	else if(MATCH("advanced", "stereo_ssb")) pconfig->stereo_ssb = atoi(value);
	else if(MATCH("advanced", "preemp_unity")) pconfig->preemp_unity_freq = strtof(value, NULL);
	else if(MATCH("advanced", "sample_rate")) pconfig->sample_rate = atoi(value);
	else if(MATCH("advanced", "lpf_cutoff")) {
		pconfig->lpf_cutoff = strtof(value, NULL);
		if(pconfig->lpf_cutoff > (pconfig->sample_rate * 0.5)) {
			pconfig->lpf_cutoff = (pconfig->sample_rate * 0.5);
			fprintf(stderr, "LPF cutoff over niquist, limiting.\n");
		}
	} else if(MATCH("advanced", "headroom")) pconfig->volumes.headroom = strtof(value, NULL);
	else if(MATCH("advanced", "drive")) pconfig->volumes.drive = strtof(value, NULL);
	else if(MATCH("advanced", "makeup")) pconfig->volumes.makeup = strtof(value, NULL);
	else if(MATCH("volumes", "pilot")) pconfig->volumes.pilot = strtof(value, NULL);
	else if(MATCH("volumes", "rds")) pconfig->volumes.rds = strtof(value, NULL);
	else if(MATCH("volumes", "rds_step")) pconfig->volumes.rds_step = strtof(value, NULL);
	else return 0; // Unknown section/name

    return 1;
}

int parse_config(FM95_Config* config, FM95_DeviceNames* dv) {
	FM95_SetupContext ctx = {
		.config = config,
		.devices = dv
	};
	return ini_parse(config->ini_config_path, &config_handler, &ctx);
}

int setup_audio(FM95_Runtime* runtime, const FM95_DeviceNames dv_names, const FM95_Config config) {
	pa_buffer_attr input_buffer_atr = {
		.maxlength = buffer_maxlength,
		.fragsize = buffer_tlength_fragsize
	};
	pa_buffer_attr output_buffer_atr = {
		.maxlength = buffer_maxlength,
		.tlength = buffer_tlength_fragsize,
		.prebuf = 512
	};

	int opentime_pulse_error;

	printf("Connecting to input device... (%s)\n", dv_names.input);
	opentime_pulse_error = init_PulseInputDevice(&runtime->input_device, config.sample_rate, 2, "fm95", "Main Audio Input", dv_names.input, &input_buffer_atr, PA_SAMPLE_FLOAT32NE);
	if (opentime_pulse_error) {
		fprintf(stderr, "Error: cannot open input device: %s\n", pa_strerror(opentime_pulse_error));
		return 1;
	}

	if(config.options.mpx_on) {
		printf("Connecting to MPX device... (%s)\n", dv_names.mpx);

		opentime_pulse_error = init_PulseInputDevice(&runtime->mpx_device, config.sample_rate, 1, "fm95", "MPX Input", dv_names.mpx, &input_buffer_atr, PA_SAMPLE_FLOAT32NE);
		if (opentime_pulse_error) {
			fprintf(stderr, "Error: cannot open MPX device: %s\n", pa_strerror(opentime_pulse_error));
			free_PulseDevice(&runtime->input_device);
			return 1;
		}
	}

	printf("Connecting to output device... (%s)\n", dv_names.output);

	opentime_pulse_error = init_PulseOutputDevice(&runtime->output_device, config.sample_rate, 1, "fm95", "MPX Output", dv_names.output, &output_buffer_atr, PA_SAMPLE_FLOAT32NE);
	if (opentime_pulse_error) {
		fprintf(stderr, "Error: cannot open output device: %s\n", pa_strerror(opentime_pulse_error));
		free_PulseDevice(&runtime->input_device);
		if(config.options.mpx_on) free_PulseDevice(&runtime->mpx_device);
		return 1;
	}
	return 0;
}

void init_runtime(FM95_Runtime* runtime, const FM95_Config config) {
	if(config.calibration != 0) {
		init_oscillator(&runtime->osc, (config.calibration == 2) ? 60 : ((config.calibration == 1) ? 400 : 19000), config.sample_rate);
		return;
	}
	else init_oscillator(&runtime->osc, 1187.5f, config.sample_rate);

	if(config.lpf_cutoff != 0) {
		runtime->lpf_l = iirfilt_rrrf_create_prototype(LIQUID_IIRDES_CHEBY2, LIQUID_IIRDES_LOWPASS, LIQUID_IIRDES_SOS, config.lpf_order, (config.lpf_cutoff/config.sample_rate), 0.0f, 1.0f, 40.0f);
		runtime->lpf_r = iirfilt_rrrf_create_prototype(LIQUID_IIRDES_CHEBY2, LIQUID_IIRDES_LOWPASS, LIQUID_IIRDES_SOS, config.lpf_order, (config.lpf_cutoff/config.sample_rate), 0.0f, 1.0f, 40.0f);
	}

	if(config.stereo_ssb) {
		for(int i = 0; i < config.rds_streams; i++) init_delay_line(&runtime->rds_delays[i], config.stereo_ssb*2);
	}

	if(config.preemphasis != 0) {
		init_preemphasis(&runtime->preemp_l, (float)config.preemphasis * 1.0e-6f, config.sample_rate, config.preemp_unity_freq);
		init_preemphasis(&runtime->preemp_r, (float)config.preemphasis * 1.0e-6f, config.sample_rate, config.preemp_unity_freq);
	}

	if(runtime->bs412.init == true && (runtime->bs412.sample_rate == config.sample_rate)) {
		reinit_bs412(&runtime->bs412, config.mpx_deviation, config.mpx_power, config.bs412_attack, config.bs412_release, config.bs412_max, config.bs412_gate, config.bs412_knee, config.bs412_strenght);
	} else init_bs412(&runtime->bs412, config.mpx_deviation, config.mpx_power, config.bs412_attack, config.bs412_release, config.bs412_max, config.bs412_gate, config.bs412_knee, config.bs412_strenght, config.sample_rate);
	init_stereo_encoder(&runtime->stencode, config.stereo_ssb, 16.0f, &runtime->osc, config.volumes.audio, config.volumes.pilot);

	float last_gain = 0.0f;
	if(config.agc_max != 0.0) {
		last_gain = 1.0f;
		if(runtime->agc.sampleRate == config.sample_rate) last_gain = runtime->agc.currentGain;
		initAGC(&runtime->agc, config.sample_rate, config.agc_target, config.agc_min, config.agc_max, config.agc_attack, config.agc_release);
		runtime->agc.currentGain = last_gain;
	}

	for(int i = 0; i < 4; i++) {
		bit_ring_init(&runtime->rds_bitring[i], 4096);
		runtime->rds_symbol[i] = -1.0f;
		runtime->rds_last_bit[i] = 0;

		runtime->rds_filter[i] = iirfilt_rrrf_create_prototype(LIQUID_IIRDES_CHEBY2, LIQUID_IIRDES_LOWPASS, LIQUID_IIRDES_SOS, 8, (2400.0f/config.sample_rate), 0.0f, 1.0f, 40.0f);
	}
}

#define BUF_SIZE 256

typedef struct {
	FM95_Runtime* runtime;
	FM95_Config* config;
	FM95_RunResult* run_result;
} FM95_Data;

static void *handle_client(ipc_client_arg_t *arg) {
    int fd = arg->client_fd;
	FM95_Data* data = arg->user_data;
    free(arg);

    char buf[BUF_SIZE];
	char reply = 0;
    ssize_t n;
	float val;

    while ((n = recv(fd, buf, sizeof(buf) - 1, 0)) > 0) {
		reply = 0xff;
        buf[n] = '\0';
		switch (buf[0]) {
			case 1:
				// Reload
				to_run = 0;
				to_reload = 1;
				reply = 0;
				break;
			case 2:
				// Quit
				to_run = 0;
				to_reload = 0;
				reply = 0;
				break;
			case 100:
				// Toggle stereo
				data->config->stereo ^= 1;
				reply = data->config->stereo;
				break;
			case 101:
				// Set makeup
				memcpy(&val, buf + 1, sizeof(float));
				data->config->volumes.makeup = val;
				reply = 0;
				break;
			case 102:
				// Set drive
				memcpy(&val, buf + 1, sizeof(float));
				data->config->volumes.drive = val;
				reply = 0;
				break;
			case 103:
				// Set audio preamp
				memcpy(&val, buf + 1, sizeof(float));
				data->config->audio_preamp = val;
				reply = 0;
				break;
			case 104:
				// Set master volume
				memcpy(&val, buf + 1, sizeof(float));
				data->config->master_volume = val;
				reply = 0;
				break;
			case 105:
				// Set BS412 gate
				memcpy(&val, buf + 1, sizeof(float));
				data->config->bs412_gate = val;
				reply = 0;
				to_run = 0;
				to_reload = 1;
				break;
			case 106:
				// Set BS412 mpx power
				memcpy(&val, buf + 1, sizeof(float));
				data->config->mpx_power = val;
				reply = 0;
				to_run = 0;
				to_reload = 1;
				break;
			case 107:
				// Set BS412 attack
				memcpy(&val, buf + 1, sizeof(float));
				data->config->bs412_attack = val;
				reply = 0;
				to_run = 0;
				to_reload = 1;
				break;
			case 108:
				// Set BS412 release
				memcpy(&val, buf + 1, sizeof(float));
				data->config->bs412_release = val;
				reply = 0;
				to_run = 0;
				to_reload = 1;
				break;
			case 109:
				// Set BS412 max
				memcpy(&val, buf + 1, sizeof(float));
				data->config->bs412_max = val;
				reply = 0;
				to_run = 0;
				to_reload = 1;
				break;
			case 110:
				// Set BS412 knee
				memcpy(&val, buf + 1, sizeof(float));
				data->runtime->bs412.knee_db = val;
				reply = 0;
				break;
			case 111:
				// Set BS412 strenght
				memcpy(&val, buf + 1, sizeof(float));
				data->runtime->bs412.strenght = val;
				reply = 0;
				break;
			case 112: {
				if (n < 2) { reply = 1; break; }
				uint8_t stream = buf[1];
				if(stream > 3) stream = 3;

				uint8_t unpacked[8 * (BUF_SIZE - 2)];
				size_t nbits = 0;
				for (ssize_t i = 2; i < n; i++) {
					for (int b = 7; b >= 0; b--)
						unpacked[nbits++] = (buf[i] >> b) & 1;
				}
				size_t written = bit_ring_write(&data->runtime->rds_bitring[stream], unpacked, nbits);
				if (written < nbits) {
					fprintf(stderr, "rds bitring overrun: dropped %zu of %zu bits\n", nbits - written, nbits);
				}
				reply = (written < nbits) ? 2 : 0;
				break;
			}
			case 0xfe:
				// Fetch config
        		send(fd, data->config, sizeof(FM95_Config), 0);
				break;
			case 0xff:
				// Fetch data
        		send(fd, data->run_result, sizeof(FM95_RunResult), 0);
				break;
			default:
				reply = 1; // Unknown command
				break;
		}

        if(reply != 0xff) send(fd, &reply, 1, 0);
    }

    printf("[client fd=%d] disconnected\n", fd);
    close(fd);
    return NULL;
}

int main(int argc, char **argv) {
	printf("fm95 (an FM Processor by radio95) version 2.6\n");

	FM95_Config config = {
		.volumes = {
			.pilot = 0.09f,
			.rds = 0.045f,
			.rds_step = 0.9f,
			.headroom = 0.05f,
			.drive = 1.0f,
			.makeup = 1.0f
		},
		.stereo = 1,
		.stereo_ssb = 0,

		.rds_streams = 1, // You have to match this with RDS95, otherwise may god have mercy on your RDS decoders

		.preemphasis = 50, // Europe, the "freedomers" use 75µs
		.calibration = 0, // Off
		.mpx_power = 3.0f, // dbr, this is for BS412, simplest bs412
		.mpx_deviation = 75000.0f, // for BS412, this is what deviation does the compressor see as peak, so if i set here 150 khz, then the compressor will act as if it was two times louder
		.audio_deviation = 75000.0f, // another way to set the volume
		.master_volume = 1.0f, // Volume of everything combined, for calibration
		.audio_preamp = 1.0f, // Volume of the audio before the filters

		.sample_rate = 192000, // Sample rate for this whole gizmo to run on

		.ini_config_path = DEFAULT_INI_PATH,

		.lpf_order = 15, // how good the lpf is, usually no more than 18 is needed
		.preemp_unity_freq = 15000.0f, // the preemphasis makes the highs louder, which for digital means no good, so instead of making the highs louder, make the lows quieter which gives the illusion of highs louder
		.agc_target = 0.625f,
		.agc_attack = 0.03f,
		.agc_release = 0.225f,
		.agc_min = 0.1f,
		.agc_max = 1.5f,
		.bs412_attack = 0.05f,
		.bs412_release = 0.025f,
		.bs412_max = 2.82f,
		.bs412_gate = -20.0f,
		.bs412_knee = 4.0f,
		.bs412_strenght = 1.0f,
		.lpf_cutoff = 15000.0f,
	};

	FM95_DeviceNames dv_names = {
		.input = "\0",
		.output = "\0",
		.mpx = "\0",
	};
	FM95_DeviceNames old_dv_names = dv_names;

	int err;
	err = parse_arguments(argc, argv, &config);
	if(err != 0) return err;

	err = parse_config(&config, &dv_names);
	if(err != 0) {
		printf("Could not parse the config file. (error code as return code)\n");
		return err;
	}

	if(dv_names.input[0] == 0) {
		printf("Please set the input device");
		return 1;
	}
	if(dv_names.output[0] == 0) {
		printf("Please set the output device");
		return 1;
	}

	if (config.volumes.drive < 0.01f) config.volumes.drive = 0.01f;

	config.master_volume *= config.audio_deviation/75000.0f;

	config.volumes.audio = calculate_sharedaudio_volume(config.volumes, config.rds_streams);

	config.options.mpx_on = (strlen(dv_names.mpx) != 0);

	FM95_Runtime runtime;
	memset(&runtime, 0, sizeof(runtime));

	err = setup_audio(&runtime, dv_names, config);
	if(err != 0) return err;

	signal(SIGINT, stop);
	signal(SIGTERM, stop);
	signal(SIGHUP, reload);

	init_runtime(&runtime, config);

	FM95_RunResult runres;
	memset(&runres, 0, sizeof(runres));
	FM95_Data fmdata = {
		.config = &config,
		.runtime = &runtime,
		.run_result = &runres
	};

	ipc_ctx_t ctx;
	ipc_ctx_t *pctx = &ctx;
	if(create_ipc(pctx, handle_client, "/etc/fm95/ctl.socket", &fmdata) < 0) {
		printf("Could not create IPC.\n");
		pctx = NULL;
	}

	int ret;
	while(true) {
		ret = run_fm95(&config, &runtime, &runres);
		if(to_reload) {
			to_reload = 0;
			printf("Reloading...\n");
			err = parse_config(&config, &dv_names);
			if(err != 0) {
				printf("Could not parse the config file. (error code as return code)\n");
				return err;
			}
			config.volumes.audio = calculate_sharedaudio_volume(config.volumes, config.rds_streams);
			if(!compare_dvs(&dv_names, &old_dv_names)) printf("Warning! Audio Device name changes are not reloaded, please restart for that to take effect.\n");
			old_dv_names = dv_names;
			cleanup_runtime(&runtime, config);
			init_runtime(&runtime, config);
			to_run = 1;
			continue;
		}
		printf("Cleaning up...\n");
		cleanup_runtime(&runtime, config);
		cleanup_audio_runtime(&runtime, config.options);
		break;
	}
	if(pctx != NULL) destroy_ipc(pctx);
	return ret;
}
