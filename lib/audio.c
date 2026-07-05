#include "audio.h"

int init_PulseInputDevice(PulseInputDevice* dev, const int sample_rate, const int channels, const char* app_name, const char *stream_name, const char* device, pa_buffer_attr* buffer_attr, enum pa_sample_format format) {
	#ifdef PULSE_DEBUG
	debug_printf("Initializing PulseInputDevice format with app_name: %s, stream_name: %s, device: %s, sample_rate: %d, channels: %d, format: %d\n", app_name, stream_name, device, sample_rate, channels, format);
	#endif

	if (dev->initialized) return PA_ERR_BADSTATE;
	pa_sample_spec sample_spec = {.format = format, .channels = channels, .rate = sample_rate};
	if (!pa_sample_spec_valid(&sample_spec)) return PA_ERR_INVALID;

	pa_buffer_attr new_buffer_attr = *buffer_attr;
	dev->sample_spec = sample_spec;
	dev->buffer_attr = new_buffer_attr;

	dev->app_name = strdup(app_name);
	dev->stream_name = strdup(stream_name);
	dev->device = strdup(device);

	dev->input = 1;

	int error;
	dev->dev = pa_simple_new(NULL, app_name, PA_STREAM_RECORD, device, stream_name, &sample_spec, NULL, &new_buffer_attr, &error);
	if (!dev->dev) return error;
	dev->initialized = 1;
	return 0;
}

int read_PulseInputDevice(PulseInputDevice* dev, void* buffer, size_t size) {
	if (!dev->initialized) return PA_ERR_BADSTATE;
	int error = 0;
	pa_simple_read(dev->dev, buffer, size, &error);
	return error;
}

void free_PulseDevice(PulseDevice* dev) {
	#ifdef PULSE_DEBUG
	debug_printf("Freeing PulseDevice with app_name: %s, stream_name: %s, device: %s, input: %d\n", dev->app_name, dev->stream_name, dev->device, dev->input);
	#endif

	if (!dev->input) pa_simple_drain(dev->dev, NULL);
	if (dev->dev && dev->initialized) pa_simple_free(dev->dev);
	free(dev->app_name);
	free(dev->stream_name);
	free(dev->device);
	dev->initialized = 0;
}


int init_PulseOutputDevice(PulseOutputDevice* dev, const int sample_rate, const int channels, const char* app_name, const char *stream_name, const char* device, pa_buffer_attr* buffer_attr, enum pa_sample_format format) {
	#ifdef PULSE_DEBUG
	debug_printf("Initializing PulseOutputDevice format with app_name: %s, stream_name: %s, device: %s, sample_rate: %d, channels: %d, format: %d\n", app_name, stream_name, device, sample_rate, channels, format);
	#endif

	if (dev->initialized) return PA_ERR_BADSTATE;
	pa_sample_spec sample_spec = {.format = format, .channels = channels, .rate = sample_rate};
	pa_buffer_attr new_buffer_attr = *buffer_attr;
	dev->sample_spec = sample_spec;
	dev->buffer_attr = new_buffer_attr;

	dev->app_name = strdup(app_name);
	dev->stream_name = strdup(stream_name);
	dev->device = strdup(device);

	dev->input = 0;

	int error;
	dev->dev = pa_simple_new(NULL, app_name, PA_STREAM_PLAYBACK, device, stream_name, &sample_spec, NULL, &new_buffer_attr, &error);
	if (!dev->dev) return error;
	dev->initialized = 1;
	return 0;
}

int write_PulseOutputDevice(PulseOutputDevice* dev, void* buffer, size_t size) {
	if (!dev->initialized) return PA_ERR_BADSTATE;
	int error = 0;
	if(pa_simple_write(dev->dev, buffer, size, &error) == 0) return 0;
	return error;
}
