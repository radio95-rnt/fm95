#include <getopt.h>
#include <liquid/liquid.h>
#include "../inih/ini.h"
#include <stdbool.h>

#define DEFAULT_INI_PATH "/etc/fm95.conf"

#define buffer_maxlength 12288
#define buffer_tlength_fragsize 12288

#include "../dsp/oscillator.h"
#include "../filter/iir.h"
#include "../modulation/stereo_encoder.h"
#include "../filter/bs412.h"
#include "../filter/gain_control.h"

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#define BUFFER_SIZE 4096 // This defines how many samples to process at a time, because the loop here is this: get signal -> process signal -> output signal, and when we get signal we actually get BUFFER_SIZE of them

#include "../io/audio.h"

#define DEFAULT_PILOT_VOLUME 0.09f // 9%
#define DEFAULT_RDS_VOLUME 0.0475f // 4.75%
#define DEFAULT_RDS_VOLUME_STEP 0.9f // 90%, so RDS2 stream 4 is 90% of stream 3 which is 90% of stream 2, which again is 90% of stream 1...

static volatile sig_atomic_t to_run = 1;
static volatile sig_atomic_t to_reload = 0;

inline float hard_clip(float sample, float threshold) { return fmaxf(-threshold, fminf(threshold, sample)); }

typedef struct
{
	bool rds_on;
	bool mpx_on;
	bool hq_on;
} FM95_Options;
typedef struct
{
	float audio;
	float headroom;
	float pilot;
	float rds;
	float rds_step;
} FM95_Volumes;
typedef struct
{
	FM95_Options options;

	FM95_Volumes volumes;
	bool stereo;

	uint8_t rds_streams;

	float clipper_threshold;
	uint8_t preemphasis;
	uint8_t calibration;
	float mpx_power;
	float mpx_deviation;
	float audio_deviation;
	float master_volume;
	float audio_volume;
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
	float lpf_cutoff;
} FM95_Config;

typedef struct
{
	PulseInputDevice input_device, mpx_device, rds_device;
	PulseOutputDevice output_device, hq_output;
	float* rds_in;
	Oscillator osc;
	iirfilt_rrrf lpf_l, lpf_r;
	ResistorCapacitor preemp_l, preemp_r;
	BS412Compressor bs412;
	StereoEncoder stencode;
	AGC agc;
	lua_State* lua;
} FM95_Runtime;

typedef struct {
    char input[64];
    char output[64];
    char hq[64];
    char mpx[64];
    char rds[64];
} FM95_DeviceNames;
typedef struct {
    FM95_Config* config;
    FM95_DeviceNames* devices;
} FM95_SetupContext;

bool compare_dvs(const FM95_DeviceNames *a, const FM95_DeviceNames *b) {
    return strcmp(a->input,  b->input)  == 0 &&
           strcmp(a->output, b->output) == 0 &&
           strcmp(a->mpx,    b->mpx)    == 0 &&
           strcmp(a->rds,    b->rds)    == 0;
}

float calculate_sharedaudio_volume(const FM95_Volumes volumes, const int rds_streams) {
	float rds_volume = volumes.rds * powf(volumes.rds_step, rds_streams);
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
		iirfilt_rrrf_destroy(runtime->lpf_l);
		iirfilt_rrrf_destroy(runtime->lpf_r);
	}
}

void cleanup_audio_runtime(FM95_Runtime *rt, const FM95_Options options) {
    free_PulseDevice(&rt->input_device);
    if (options.hq_on) free_PulseDevice(&rt->hq_output);
    if (options.mpx_on) free_PulseDevice(&rt->mpx_device);
    if (options.rds_on) {
		free_PulseDevice(&rt->rds_device);
		free(rt->rds_in);
	}
    free_PulseDevice(&rt->output_device);
}

int run_fm95(const FM95_Config config, FM95_Runtime* runtime) {
	float output[BUFFER_SIZE];
	float output_hq[BUFFER_SIZE*2];

	int pulse_error;

	if(config.calibration != 0) {
		while(to_run) {
			for (int i = 0; i < BUFFER_SIZE; i++) {
				float sample = get_oscillator_sin_sample(&runtime->osc);
				if(config.calibration == 2) sample = (sample > 0.0f) ? 1.0f : -1.0f; // Sine wave to square wave filter, 50% duty cycle
				output[i] = sample*config.master_volume;
			}
			if((pulse_error = write_PulseOutputDevice(&runtime->output_device, output, sizeof(output)))) { // get output from the function and assign it into pulse_error, this comment to avoid confusion
				fprintf(stderr, "Error writing to output device: %s\n", pa_strerror(pulse_error));
				to_run = 0;
				break;
			}
		}
		return 0;
	}

	float audio_stereo_input[BUFFER_SIZE*2]; // Stereo

	float mpx_in[BUFFER_SIZE] = {0};

	bool mpx_on = config.options.mpx_on;
	bool rds_on = config.options.rds_on;

	int script_ref = 0;
	// Load the file
	if (luaL_loadfile(runtime->lua, "/home/user/test.lua") == LUA_OK) {
		// luaL_ref pops the function from the stack and returns a unique integer ID
		script_ref = luaL_ref(runtime->lua, LUA_REGISTRYINDEX);
	}

	while (to_run) {
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
		if(rds_on) {
			if((pulse_error = read_PulseInputDevice(&runtime->rds_device, runtime->rds_in, sizeof(float) * BUFFER_SIZE * config.rds_streams))) {
				fprintf(stderr, "Error reading from RDS95 device: %s\nDisabling RDS.\n", pa_strerror(pulse_error));
				rds_on = 0;
			}
		}

		for (uint16_t i = 0; i < BUFFER_SIZE; i++) {
			float mpx = 0.0f;

			float l = audio_stereo_input[2*i+0]*config.audio_preamp;
			float r = audio_stereo_input[2*i+1]*config.audio_preamp;
			
			if(config.agc_max != 0.0) {
				float agc_gain = process_agc(&runtime->agc, 0.5f * (fabsf(l) + fabsf(r)));
				l *= agc_gain;
				r *= agc_gain;
			}

			if (config.clipper_threshold != 0) {
				l = hard_clip(l * config.audio_volume, config.clipper_threshold);
				r = hard_clip(r * config.audio_volume, config.clipper_threshold);
			}

			float mod_l, mod_r;

			if(config.lpf_cutoff != 0) {
				iirfilt_rrrf_execute(runtime->lpf_l, l, &mod_l);
				iirfilt_rrrf_execute(runtime->lpf_r, r, &mod_r);
			}
			
			if(config.preemphasis != 0) {
				mod_l = apply_preemphasis(&runtime->preemp_l, mod_l);
				mod_r = apply_preemphasis(&runtime->preemp_r, mod_r);
			}

			lua_rawgeti(L, LUA_REGISTRYINDEX, runtime->script_ref);
			lua_pushnumber(L, mod_l); // Argument 1
			lua_pushnumber(L, mod_r); // Argument 2

			if (lua_pcall(L, 2, 2, 0) == LUA_OK) { // 2 args, 2 results
				mod_r = lua_tonumber(L, -1);
				mod_l = lua_tonumber(L, -2);
				lua_pop(L, 2);
			}

			mpx = stereo_encode(&runtime->stencode, config.stereo, mod_l, mod_r);

			if(rds_on) {
				float rds_level = config.volumes.rds;
				for(uint8_t stream = 0; stream < config.rds_streams; stream++) {
					uint8_t osc_stream = 12 + stream;
					if(osc_stream >= 13) osc_stream++;

					mpx += (runtime->rds_in[config.rds_streams * i + stream] * get_oscillator_cos_multiplier_ni(&runtime->osc, osc_stream)) * rds_level;

					rds_level *= config.volumes.rds_step; // Prepare level for the next stream
				}
			}

			mpx = bs412_compress(&runtime->bs412, mpx+mpx_in[i]);

			output[i] = hard_clip(mpx*config.master_volume, 1.0); // Ensure peak deviation of 75 khz (or the set deviation), assuming we're calibrated correctly
			advance_oscillator(&runtime->osc);

			output_hq[2*i+0] = hard_clip(l, 1.0f);
			output_hq[2*i+1] = hard_clip(r, 1.0f);
		}

		if((pulse_error = write_PulseOutputDevice(&runtime->output_device, output, sizeof(output)))) {
			fprintf(stderr, "Error writing to output device: %s\n", pa_strerror(pulse_error));
			to_run = 0;
			break;
		}
		if(config.options.hq_on) {
			if((pulse_error = write_PulseOutputDevice(&runtime->hq_output, output_hq, sizeof(output_hq)))) {
				fprintf(stderr, "Error writing to HQ output device: %s\n", pa_strerror(pulse_error));
				to_run = 0;
				break;
			}
		}
		lua_gc(runtime->lua, LUA_GCSTEP);
	}
	luaL_unref(runtime->lua, LUA_REGISTRYINDEX, script_ref);

	return 0;
}


int parse_arguments(int argc, char **argv, FM95_Config* config) {
	int opt;
	const char	*short_opt = "c:h";
	struct option	long_opt[] =
	{
		{"config",		required_argument,	NULL,	'c'},
		{"help",        no_argument,       NULL, 'h'},
		{0,             0,                 0,    0}
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

    if (MATCH("fm95", "stereo")) {
        pconfig->stereo = atoi(value);
	} else if (MATCH("devices", "input")) {
        strncpy(dv->input, value, 63);
        dv->input[63] = '\0';
    } else if (MATCH("devices", "output")) {
        strncpy(dv->output, value, 63);
        dv->output[63] = '\0';
	} else if (MATCH("devices", "hq")) {
        strncpy(dv->hq, value, 63);
        dv->hq[63] = '\0';
    } else if (MATCH("devices", "mpx")) {
        strncpy(dv->mpx, value, 63);
        dv->mpx[63] = '\0';
    } else if (MATCH("devices", "rds")) {
        strncpy(dv->rds, value, 63);
        dv->rds[63] = '\0';
    } else if (MATCH("fm95", "rds_streams")) {
        pconfig->rds_streams = atoi(value);
        if(pconfig->rds_streams > 4) {
            printf("RDS Streams more than 4? Nuh uh\n");
            return 0;
        }
    } else if (MATCH("fm95", "clipper_threshold")) {
        pconfig->clipper_threshold = strtof(value, NULL);
    } else if (MATCH("fm95", "preemphasis")) {
        pconfig->preemphasis = atoi(value);
    } else if (MATCH("fm95", "calibration")) {
        pconfig->calibration = atoi(value);
    } else if (MATCH("fm95", "mpx_power")) {
        pconfig->mpx_power = strtof(value, NULL);
    } else if (MATCH("fm95", "mpx_deviation")) {
        pconfig->mpx_deviation = strtof(value, NULL);
    } else if (MATCH("fm95", "master_volume")) {
        pconfig->master_volume = strtof(value, NULL);
    } else if (MATCH("fm95", "audio_volume")) {
        pconfig->audio_volume = strtof(value, NULL);
    } else if (MATCH("fm95", "audio_preamp")) {
        pconfig->audio_preamp = strtof(value, NULL);
    } else if (MATCH("fm95", "deviation")) {
        pconfig->audio_deviation = strtof(value, NULL);
	} else if(MATCH("fm95", "bs412_max")) {
		pconfig->bs412_max = strtof(value, NULL);
	} else if(MATCH("fm95", "agc_target")) {
		pconfig->agc_target = strtof(value, NULL);
	} else if(MATCH("fm95", "agc_attack")) {
		pconfig->agc_attack = strtof(value, NULL);
	} else if(MATCH("fm95", "agc_release")) {
		pconfig->agc_release = strtof(value, NULL);
	} else if(MATCH("fm95", "agc_min")) {
		pconfig->agc_min = strtof(value, NULL);
	} else if(MATCH("fm95", "agc_max")) {
		pconfig->agc_max = strtof(value, NULL);
	} else if(MATCH("fm95", "bs412_attack")) {
		pconfig->bs412_attack = strtof(value, NULL);
	} else if(MATCH("fm95", "bs412_release")) {
		pconfig->bs412_release = strtof(value, NULL);
	} else if(MATCH("advanced", "lpf_order")) {
		pconfig->lpf_order = atoi(value);
	} else if(MATCH("advanced", "preemp_unity")) {
		pconfig->preemp_unity_freq = strtof(value, NULL);
	} else if(MATCH("advanced", "sample_rate")) {
		pconfig->sample_rate = atoi(value);
	} else if(MATCH("advanced", "lpf_cutoff")) {
		pconfig->lpf_cutoff = strtof(value, NULL);
		if(pconfig->lpf_cutoff > (pconfig->sample_rate * 0.5)) {
			pconfig->lpf_cutoff = (pconfig->sample_rate * 0.5);
			fprintf(stderr, "LPF cutoff over niquist, limiting.\n");
		}
	} else if(MATCH("advanced", "headroom")) {
		pconfig->volumes.headroom = strtof(value, NULL);
	} else if(MATCH("volumes", "pilot")) {
		pconfig->volumes.pilot = strtof(value, NULL);
	} else if(MATCH("volumes", "rds")) {
		pconfig->volumes.rds = strtof(value, NULL);
	} else if(MATCH("volumes", "rds_step")) {
		pconfig->volumes.rds_step = strtof(value, NULL);
	} else {
        return 0; // Unknown section/name
    }

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
		.prebuf = 64
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
	if(config.options.rds_on) {
		printf("Connecting to RDS95 device... (%s)\n", dv_names.rds);

		opentime_pulse_error = init_PulseInputDevice(&runtime->rds_device, config.sample_rate, config.rds_streams, "fm95", "RDS95 Input", dv_names.rds, &input_buffer_atr, PA_SAMPLE_FLOAT32NE);
		if (opentime_pulse_error) {
			fprintf(stderr, "Error: cannot open RDS device: %s\n", pa_strerror(opentime_pulse_error));
			free_PulseDevice(&runtime->input_device);
			if(config.options.mpx_on) free_PulseDevice(&runtime->mpx_device);
			return 1;
		}
		runtime->rds_in = malloc(sizeof(float) * BUFFER_SIZE * config.rds_streams);
	}

	printf("Connecting to output device... (%s)\n", dv_names.output);

	opentime_pulse_error = init_PulseOutputDevice(&runtime->output_device, config.sample_rate, 1, "fm95", "MPX Output", dv_names.output, &output_buffer_atr, PA_SAMPLE_FLOAT32NE);
	if (opentime_pulse_error) {
		fprintf(stderr, "Error: cannot open output device: %s\n", pa_strerror(opentime_pulse_error));
		free_PulseDevice(&runtime->input_device);
		if(config.options.mpx_on) free_PulseDevice(&runtime->mpx_device);
		if(config.options.rds_on) {
    	    free_PulseDevice(&runtime->rds_device);
	        free(runtime->rds_in);
    	}
		return 1;
	}

	if(config.options.hq_on) {
		printf("Connecting to HQ device... (%s)\n", dv_names.hq);

		opentime_pulse_error = init_PulseOutputDevice(&runtime->hq_output, config.sample_rate, 2, "fm95", "Audio output", dv_names.hq, &output_buffer_atr, PA_SAMPLE_FLOAT32NE);
		if (opentime_pulse_error) {
			fprintf(stderr, "Error: cannot open HQ device: %s\n", pa_strerror(opentime_pulse_error));
			free_PulseDevice(&runtime->input_device);
			free_PulseDevice(&runtime->output_device);
			if(config.options.mpx_on) free_PulseDevice(&runtime->mpx_device);
			if(config.options.rds_on) {
    	    	free_PulseDevice(&runtime->rds_device);
		        free(runtime->rds_in);
    		}
			return 1;
		}
	}
	return 0;
}

void init_runtime(FM95_Runtime* runtime, const FM95_Config config) {	
	if(config.calibration != 0) {
		init_oscillator(&runtime->osc, (config.calibration == 2) ? 60 : 400, config.sample_rate);
		return;
	}
	else init_oscillator(&runtime->osc, 4750, config.sample_rate);

	if(config.lpf_cutoff != 0) {
		runtime->lpf_l = iirfilt_rrrf_create_prototype(LIQUID_IIRDES_CHEBY2, LIQUID_IIRDES_LOWPASS, LIQUID_IIRDES_SOS, config.lpf_order, (config.lpf_cutoff/config.sample_rate), 0.0f, 1.0f, 60.0f);
		runtime->lpf_r = iirfilt_rrrf_create_prototype(LIQUID_IIRDES_CHEBY2, LIQUID_IIRDES_LOWPASS, LIQUID_IIRDES_SOS, config.lpf_order, (config.lpf_cutoff/config.sample_rate), 0.0f, 1.0f, 60.0f);
	}

	if(config.preemphasis != 0) {
		init_preemphasis(&runtime->preemp_l, (float)config.preemphasis * 1.0e-6f, config.sample_rate, config.preemp_unity_freq);
		init_preemphasis(&runtime->preemp_r, (float)config.preemphasis * 1.0e-6f, config.sample_rate, config.preemp_unity_freq);
	}

	float last_gain = 0.0f;
	if(runtime->bs412.sample_rate == config.sample_rate) last_gain = runtime->bs412.gain;
	init_bs412(&runtime->bs412, config.mpx_deviation, config.mpx_power, config.bs412_attack, config.bs412_release, config.bs412_max, config.sample_rate);
	runtime->bs412.gain = last_gain;

	init_stereo_encoder(&runtime->stencode, 4.0f, &runtime->osc, config.volumes.audio, config.volumes.pilot);

	if(config.agc_max != 0.0) {
		last_gain = 1.0f;
		if(runtime->agc.sampleRate == config.sample_rate) last_gain = runtime->agc.currentGain;
		initAGC(&runtime->agc, config.sample_rate, config.agc_target, config.agc_min, config.agc_max, config.agc_attack, config.agc_release);
		runtime->agc.currentGain = last_gain;
	}

	if(config.options.rds_on) memset(runtime->rds_in, 0, sizeof(float) * BUFFER_SIZE * config.rds_streams);
}

int main(int argc, char **argv) {
	printf("fm95 (an FM Processor by radio95) version 2.3\n");

	FM95_Config config = {
		.volumes = {
			.pilot = DEFAULT_PILOT_VOLUME,
			.rds = DEFAULT_RDS_VOLUME,
			.rds_step = DEFAULT_RDS_VOLUME_STEP,
			.headroom = 0.05f
		},
		.stereo = 1,

		.rds_streams = 1, // You have to match this with RDS95, otherwise may god have mercy on your RDS decoders

		.clipper_threshold = 1.0f, // At what level for the clipper to work, 1.0f, clips the audio at 1 volt peak to peak, so it will be always between -1 and 1
		.preemphasis = 50, // Europe, the "freedomers" use 75µs
		.calibration = 0, // Off
		.mpx_power = 3.0f, // dbr, this is for BS412, simplest bs412
		.mpx_deviation = 75000.0f, // for BS412, this is what deviation does the compressor see as peak, so if i set here 150 khz, then the compressor will act as if it was two times louder
		.audio_deviation = 75000.0f, // another way to set the volume
		.master_volume = 1.0f, // Volume of everything combined, for calibration
		.audio_volume = 1.0f, // Volume of the audio, before stereo encoding, before clipper
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
		.bs412_release = 0.025,
		.bs412_max = 1.0f,
		.lpf_cutoff = 15000,
	};

	FM95_DeviceNames dv_names = {
		.input = "\0",
		.output = "\0",
		.mpx = "\0",
		.rds = "\0",
		.hq = "\0"
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

	if(strlen(dv_names.input) == 0) {
		printf("Please set the input device");
		return 1;
	}
	if(strlen(dv_names.output) == 0) {
		printf("Please set the output device");
		return 1;
	}

	config.master_volume *= config.audio_deviation/75000.0f;

	config.volumes.audio = calculate_sharedaudio_volume(config.volumes, config.rds_streams);

	FM95_Runtime runtime;
	memset(&runtime, 0, sizeof(runtime));
	runtime.lua = luaL_newstate();
	lua_gc(runtime.lua, LUA_GCSTOP);

	config.options.mpx_on = (strlen(dv_names.mpx) != 0);
	config.options.rds_on = (strlen(dv_names.rds) != 0 && config.rds_streams != 0);
	config.options.hq_on = (strlen(dv_names.hq) != 0);

	err = setup_audio(&runtime, dv_names, config);
	if(err != 0) return err;

	signal(SIGINT, stop);
	signal(SIGTERM, stop);
	signal(SIGHUP, reload);

	init_runtime(&runtime, config);

	int ret;
	while(true) {
		ret = run_fm95(config, &runtime);
		if(to_reload) {
			to_reload = 0;
			printf("Reloading...\n");
			uint8_t old_streams = config.rds_streams; // keep the rds streams
			err = parse_config(&config, &dv_names);
			if(err != 0) {
				printf("Could not parse the config file. (error code as return code)\n");
				return err;
			}
			config.volumes.audio = calculate_sharedaudio_volume(config.volumes, config.rds_streams);
			if(!compare_dvs(&dv_names, &old_dv_names)) printf("Warning! Audio Device name changes are not reloaded, please restart for that to take effect.\n");
			old_dv_names = dv_names;
			if(config.rds_streams != old_streams) printf("Warning! change of rds_streams requires a restart, not a reload.\n");
			config.rds_streams = old_streams;
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
	return ret;
}
