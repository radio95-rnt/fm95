#include <pulse/simple.h>

#define VBAN_SR_MAXNUMBER 21
static long VBAN_SRList[VBAN_SR_MAXNUMBER] = {
    6000, 12000, 24000, 48000, 96000, 192000, 384000,
    8000, 16000, 32000, 64000, 128000, 256000, 512000,
    11025, 22050, 44100, 88200, 176400, 352800, 705600
};

#define VBAN_BIT_MAXNUMBER 5 // 7 in the standard but pa does these 5
static enum pa_sample_format VBAN_BITList[VBAN_BIT_MAXNUMBER] = {
    PA_SAMPLE_U8,
    PA_SAMPLE_S16NE,
    PA_SAMPLE_S24NE,
    PA_SAMPLE_S32NE,
    PA_SAMPLE_FLOAT32NE,
};
static char VBAN_TextBITList[VBAN_BIT_MAXNUMBER][4] = {
    "U08",
    "S16",
    "S24",
    "S32",
    "F32",
};

#define VBAN_PROTOCOL_AUDIO 0x00
#define VBAN_PROTOCOL_SERIAL 0x20
#define VBAN_PROTOCOL_TXT 0x40
#define VBAN_PROTOCOL_SERVICE 0x60

#define VBAN_SERVICE_IDENTIFICATION 0
#define VBAN_SERVICE_CHATUTF8 1
#define VBAN_SERVICE_RTPACKETREGISTER 32
#define VBAN_SERVICE_RTPACKET 33

#define VBANPING_TYPE_RECEPTOR 0x00000001 // Simple receptor
#define VBANPING_TYPE_TRANSMITTER 0x00000002 // Simple Transmitter
#define VBANPING_TYPE_RECEPTORSPOT 0x00000004 // SPOT receptor (able to receive several streams)
#define VBANPING_TYPE_TRANSMITTERSPOT 0x00000008 // SPOT transmitter (able to send several streams)
#define VBANPING_TYPE_VIRTUALDEVICE 0x00000010 // Virtual Device
#define VBANPING_TYPE_VIRTUALMIXER 0x00000020 // Virtual Mixer
#define VBANPING_TYPE_MATRIX 0x00000040 // MATRIX
#define VBANPING_TYPE_DAW 0x00000080 // Workstation
#define VBANPING_TYPE_SERVER 0x01000000 // VBAN SERVER

#define VBANPING_FEATURE_AUDIO 0x00000001
#define VBANPING_FEATURE_AOIP 0x00000002
#define VBANPING_FEATURE_VOIP 0x00000004
#define VBANPING_FEATURE_SERIAL 0x00000100
#define VBANPING_FEATURE_MIDI 0x00000300
#define VBANPING_FEATURE_FRAME 0x00001000
#define VBANPING_FEATURE_TXT 0x00010000

#pragma pack(1)
typedef struct {
    char vban[4];
    uint8_t protocol_sample_rate_idx; // format_SR
    uint8_t samples_per_frame; // format_nbs
    uint8_t sample_channels; // format_nbc
    uint8_t format_type; // format_bit
    char streamname[16];
    uint32_t frame_num; // nuFrame
} VBANHeader;

typedef union {
    VBANHeader packet_data;
    char raw_data[sizeof(VBANHeader)];
} VBANHeaderUnion;

typedef struct {
    uint32_t bitType;
    uint32_t bitfeature;
    uint32_t bitfeatureEx;
    uint32_t PreferredRate;
    uint32_t MinRate;
    uint32_t MaxRate;
    uint32_t colorRGB;
    uint8_t nVersion[4];

    char GPS_Position[8];
    char USER_Position[8];
    char LangCode_ascii[8];
    char reserved_ascii[8];

    char reservedEx[64];
    char DistantIP_ascii[32];
    uint16_t DistantPort;
    uint16_t DistantReserved;

    char DeviceName_ascii[64];
    char ManufacturerName_ascii[64];
    char ApplicationName_ascii[64];
    char HostName_ascii[64];
    char UserName_utf8[128];
    char UserComment_utf8[128];
} VBANPing0Data;

typedef union {
    VBANPing0Data data;
    char raw_data[sizeof(VBANPing0Data)];
} VBANPing0DataUnion;
#pragma pack()