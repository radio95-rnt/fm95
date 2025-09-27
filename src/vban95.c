#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <getopt.h>
#include <pwd.h>
#include <fcntl.h>
#include <errno.h>

#define buffer_maxlength 12288
#define buffer_tlength_fragsize 12288
#define buffer_prebuf 64

#include "../io/audio.h"
#include "../lib/debug.h"
#include "../lib/vban.h"

#define BUF_SIZE 1500
#define MAX_AUDIO_DATA_SIZE (BUF_SIZE - sizeof(VBANHeader))
#define MAX_BUFFER_PACKETS 24

#define POLL_TIMEOUT_MS 75

typedef struct {
    char data[MAX_AUDIO_DATA_SIZE];
    size_t size;
} AudioPacket;

typedef struct {
    AudioPacket* packets;
    int capacity;
    int head;
    int tail;
    int count;
    VBANHeader header;
} AudioBuffer;

AudioBuffer* create_audio_buffer(int capacity) {
    AudioBuffer* buffer = (AudioBuffer*)malloc(sizeof(AudioBuffer));
    if (!buffer) {
        perror("Failed to allocate audio buffer");
        return NULL;
    }

    buffer->packets = (AudioPacket*)malloc(capacity * sizeof(AudioPacket));
    if (!buffer->packets) {
        perror("Failed to allocate packet buffer");
        free(buffer);
        return NULL;
    }

    buffer->capacity = capacity;
    buffer->head = 0;
    buffer->tail = 0;
    buffer->count = 0;

    return buffer;
}

void destroy_audio_buffer(AudioBuffer* buffer) {
    if (buffer) {
        free(buffer->packets);
        free(buffer);
    }
}

int add_to_buffer(AudioBuffer* buffer, const char* data, size_t size, const VBANHeader* header) {
    if (size > MAX_AUDIO_DATA_SIZE) {
        fprintf(stderr, "Audio data too large for buffer\n");
        return -1;
    }

    if (buffer->count == buffer->capacity) {
        buffer->tail = (buffer->tail + 1) % buffer->capacity;
        buffer->count--;
    }

    AudioPacket* pkt = &buffer->packets[buffer->head];
    memcpy(pkt->data, data, size);
    pkt->size = size;
    memcpy(&buffer->header, header, sizeof(VBANHeader));

    buffer->head = (buffer->head + 1) % buffer->capacity;
    buffer->count++;

    return 1;
}

volatile uint8_t to_run = 1;

static void stop(int signum) {
    (void)signum;
    printf("\nReceived stop signal.\n");
    to_run = 0;
}

static PulseOutputDevice output = {0};

void process_audio_buffer(AudioBuffer* buffer, PulseOutputDevice* output_device) {
    while (buffer->count > 0) {
        AudioPacket* pkt = &buffer->packets[buffer->tail];
        write_PulseOutputDevice(output_device, pkt->data, pkt->size);

        buffer->tail = (buffer->tail + 1) % buffer->capacity;
        buffer->count--;
    }
}

void reset_audio_buffer(AudioBuffer* buffer) {
    buffer->head = 0;
    buffer->tail = 0;
    buffer->count = 0;
}

void show_version() {
	printf("vban95 (a VBAN AOIP receiver by radio95) version 1.1\n");
}
void show_help(char *name) {
    printf(
        "Usage: \t%s\n"
        "\t-i,--ip\t\tOverride remote IP address\n"
        "\t-p,--port\tOverride listen port\n"
        "\t-s,--stream\tOverride stream name\n"
        "\t-b,--buffer\tOverride buffer size (1 to %d)\n"
        "\t-d,--device\tOverride PulseAudio device\n"
        "\t-q,--quiet\tSuppress output messages\n",
        name, MAX_BUFFER_PACKETS
    );
}

int main(int argc, char *argv[]) {
    show_version();

    char *remote_ip = "0.0.0.0";
    int listen_port = 6980;
    char *stream_name = "VBAN";
    int buffer_size = 8;
    char *pulse_device = "";
    int quiet = 0;
    
    int opt;
    const char *short_opt = "i:p:s:b:d:qh";
    const struct option long_opt[] = {
        {"ip", required_argument, NULL, 'i'},
        {"port", required_argument, NULL, 'p'},
        {"stream", required_argument, NULL, 's'},
        {"buffer", required_argument, NULL, 'b'},
        {"device", required_argument, NULL, 'd'},
        {"quiet", no_argument, NULL, 'q'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };
    while ((opt = getopt_long(argc, argv, short_opt, long_opt, NULL)) != -1) {
        switch (opt) {
            case 'i':
                remote_ip = optarg;
                break;
            case 'p':
                listen_port = atoi(optarg);
                break;
            case 's':
                stream_name = optarg;
                break;
            case 'b':
                buffer_size = atoi(optarg);
                break;
            case 'd':
                pulse_device = optarg;
                break;
            case 'q':
                quiet = 1;
                break;
            case 'h':
                show_help(argv[0]);
                return 0;
            default:
                show_help(argv[0]);
                return 1;
        }
    }

    if (buffer_size <= 0 || buffer_size > MAX_BUFFER_PACKETS) {
        fprintf(stderr, "Buffer size must be between 1 and %d\n", MAX_BUFFER_PACKETS);
        return 1;
    }

    printf("Starting VBAN receiver with buffer size: %d packets\n", buffer_size);

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return 1;
    }

    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl(F_GETFL)");
        return -1;
    }

    if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl(F_SETFL)");
        return -1;
    }

    struct sockaddr_in local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    local_addr.sin_port = htons(listen_port);

    if (bind(sockfd, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
        perror("bind");
        close(sockfd);
        return 1;
    }

    struct in_addr remote_addr_bin;
    if (inet_pton(AF_INET, remote_ip, &remote_addr_bin) != 1) {
        fprintf(stderr, "Invalid remote IP address: %s\n", remote_ip);
        close(sockfd);
        return 1;
    }

    char buffer[BUF_SIZE];
    struct sockaddr_in sender_addr;
    socklen_t sender_len = sizeof(sender_addr);

    // uint32_t vban_frame = 0;
    uint8_t vban_last_sr = 0;
    uint8_t vban_last_format = 0;
    uint8_t vban_last_channels = 0;
    uint8_t vban_audio_reset = 0;

    AudioBuffer* audio_buffer = create_audio_buffer(buffer_size);
    if (!audio_buffer) {
        close(sockfd);
        return 1;
    }

    pa_buffer_attr buffer_attr = {
        .maxlength = buffer_maxlength,
        .tlength = buffer_tlength_fragsize,
        .prebuf = buffer_prebuf
    };

    signal(SIGINT, stop);
    signal(SIGTERM, stop);

    while (to_run) {
        ssize_t recv_len = recvfrom(sockfd, buffer, BUF_SIZE, 0, (struct sockaddr *)&sender_addr, &sender_len);
        if (recv_len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(POLL_TIMEOUT_MS * 1000);
                continue;
            } else {
                perror("recvfrom error");
                break;
            }
        }

        if ((size_t)recv_len < sizeof(VBANHeader)) continue;

        if (sender_addr.sin_addr.s_addr == remote_addr_bin.s_addr || remote_addr_bin.s_addr == 0) {
            VBANHeaderUnion data;
            memcpy(&data.raw_data, buffer, sizeof(VBANHeader));

            if (memcmp(data.packet_data.vban, "VBAN", 4) != 0) continue;

            uint8_t protocol = data.packet_data.protocol_sample_rate_idx & 0xe0;
            if(protocol != VBAN_PROTOCOL_AUDIO) {
                if(protocol == VBAN_PROTOCOL_SERVICE) {
                    // Handle Service protocol
                    uint8_t service_type = data.packet_data.sample_channels;
                    uint8_t service_function = data.packet_data.samples_per_frame; // 0 if ping, 80 if reply

                    if(service_type == VBAN_SERVICE_IDENTIFICATION) {
                        if(service_function == 0) {
                            // Handle ping
                            VBANPing0DataUnion ping_data;
                            memset(&ping_data, 0, sizeof(VBANPing0Data));

                            ping_data.data.bitType = VBANPING_TYPE_RECEPTOR;
                            ping_data.data.bitfeature = VBANPING_FEATURE_AUDIO | VBANPING_FEATURE_AOIP;
                            ping_data.data.nVersion[0] = 1;
                            ping_data.data.nVersion[1] = 1;

                            snprintf(ping_data.data.DistantIP_ascii, sizeof(ping_data.data.DistantIP_ascii), "%s", inet_ntoa(sender_addr.sin_addr));
                            ping_data.data.DistantPort = htons(listen_port);
                            strncpy(ping_data.data.ApplicationName_ascii, "vban95", sizeof(ping_data.data.ApplicationName_ascii));

                            uid_t uid = getuid();
                            struct passwd *pw = getpwuid(uid);
                            if (pw != NULL) snprintf(ping_data.data.UserName_utf8, sizeof(ping_data.data.UserName_utf8), "%s", pw->pw_name);

                            gethostname(ping_data.data.HostName_ascii, sizeof(ping_data.data.HostName_ascii));

                            VBANHeaderUnion reply_header;
                            memset(&reply_header, 0, sizeof(VBANHeader));

                            memcpy(reply_header.packet_data.vban, "VBAN", 4);
                            reply_header.packet_data.protocol_sample_rate_idx = VBAN_PROTOCOL_SERVICE;
                            reply_header.packet_data.sample_channels = VBAN_SERVICE_IDENTIFICATION;
                            reply_header.packet_data.samples_per_frame = 0x80; // reply
                            reply_header.packet_data.frame_num = data.packet_data.frame_num;

                            char reply_buffer[sizeof(VBANHeader) + sizeof(VBANPing0Data)];
                            memcpy(reply_buffer, &reply_header.raw_data, sizeof(VBANHeader));
                            memcpy(reply_buffer + sizeof(VBANHeader), &ping_data.raw_data, sizeof(VBANPing0Data));
                            ssize_t sent_len = sendto(sockfd, reply_buffer, sizeof(reply_buffer), 0,
                                                      (struct sockaddr *)&sender_addr, sender_len);
                            if (sent_len < 0) {
                                perror("sendto");
                            } else {
                                if (quiet == 0) printf("Sent VBAN ping reply to %s:%d\n", inet_ntoa(sender_addr.sin_addr), ntohs(sender_addr.sin_port));
                            }
                        }
                    }
                }
                continue;
            }

            if (strncmp(data.packet_data.streamname, stream_name, sizeof(data.packet_data.streamname)) != 0) continue;
            
            char* audio_data = buffer + sizeof(VBANHeader);
            size_t audio_data_size = recv_len - sizeof(VBANHeader);
#if 0
            if (vban_frame == 0) {
                vban_frame = data.packet_data.frame_num;
            } else {
                int32_t diff = (int32_t)(data.packet_data.frame_num - (vban_frame++) - 1);
                if(diff != 0) {
                    debug_printf("Frame number diff: %d\n", diff);
                    if(diff == 0) {
                        if (quiet == 0) printf("Duplicate packet received\n");
                    } else if (diff > 1) {
                        if (quiet == 0) printf("Dropped %u packets\n", diff);
                        
                        AudioPacket blank_packet;
                        uint8_t fill_value = (data.packet_data.format_type == 0) ? 0 : 128;
                        memset(blank_packet.data, fill_value, audio_data_size);
                        blank_packet.size = audio_data_size;

                        VBANHeaderUnion temp;
                        memset(blank_packet.data, 0, blank_packet.size);
                        memcpy(&temp.raw_data, buffer, sizeof(VBANHeader));

                        for (uint32_t i = diff; i < temp.packet_data.frame_num; i++) {
                            temp.packet_data.frame_num = i;
                            add_to_buffer(audio_buffer, blank_packet.data, blank_packet.size, &temp.packet_data);
                        }
                    } else if (diff < 1) {
                        if (quiet == 0) printf("Packets received out of order (got:%u, expected:%u)\n", 
                                            data.packet_data.frame_num, vban_frame);
                    }
                    vban_frame = data.packet_data.frame_num;
                }
            }
#endif

            uint8_t actual_sr_idx = data.packet_data.protocol_sample_rate_idx & 0x1f;
            if(vban_last_sr != actual_sr_idx) {
                vban_last_sr = actual_sr_idx;
                if(quiet == 0) printf("New sample rate of %ld\n", VBAN_SRList[vban_last_sr % VBAN_SR_MAXNUMBER]);
                vban_audio_reset = 1;
                reset_audio_buffer(audio_buffer);
            }
            
            if(vban_last_format != data.packet_data.format_type) {
                vban_last_format = data.packet_data.format_type;
                if(quiet == 0) printf("New data format of %s\n", VBAN_TextBITList[vban_last_format % VBAN_BIT_MAXNUMBER]); // Here it should be fine to use the modulo, as during the reset we point out the idx may be shit
                vban_audio_reset = 1;
                reset_audio_buffer(audio_buffer);
            }
            
            if(vban_last_channels != data.packet_data.sample_channels) {
                vban_last_channels = data.packet_data.sample_channels;
                if(quiet == 0) printf("New channel count of %d\n", vban_last_channels + 1); // Add 1 because VBAN channels are 0-based
                vban_audio_reset = 1;
                reset_audio_buffer(audio_buffer);
            }

            if(vban_audio_reset) {
                if (vban_last_sr >= VBAN_SR_MAXNUMBER || vban_last_format >= VBAN_BIT_MAXNUMBER) {
                    fprintf(stderr, "Unsupported sample rate or format\n");
                    continue;
                }

                if (output.initialized) free_PulseDevice(&output);
                
                int result = init_PulseOutputDevice(
                    &output, 
                    VBAN_SRList[vban_last_sr], 
                    vban_last_channels + 1, // Add 1 because VBAN channels are 0-based
                    "vban95", 
                    stream_name, 
                    pulse_device, 
                    &buffer_attr,
                    VBAN_BITList[vban_last_format]
                );
                
                if (result != 0) fprintf(stderr, "Failed to initialize PulseAudio output device: %s\n", pa_strerror(result));
                
                vban_audio_reset = 0;
                continue;
            }

            if (add_to_buffer(audio_buffer, audio_data, audio_data_size, &data.packet_data) > 0) process_audio_buffer(audio_buffer, &output);
        }
    }

    // Clean up
    printf("Cleaning up...\n");
    if (output.initialized) free_PulseDevice(&output);
    destroy_audio_buffer(audio_buffer);
    close(sockfd);
    
    return 0;
}