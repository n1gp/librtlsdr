#ifndef _RTL_HPSDR_H
#define _RTL_HPSDR_H

/* Copyright (C)
*
* 2025 - Rick Koch, N1GP
*   Rewrote rtl_hpsdr using Christoph's hpsdrsim code as a guide
*   for discovery and start/stop of data.
*
*   This program is free software: you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation, either version 3 of the License, or
*   (at your option) any later version.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program.  If not, see <https://www.gnu.org/licenses/>.
*
*/

//
// hpsdrsim.h, define global data
//
// From the main program, this is included with EXTERN="", while
// other modules include is with "EXTERN=extern".
//

#include <stdlib.h>

#ifndef _WIN32
#include <stdio.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <net/if_arp.h>
#include <net/if.h>
#include <netdb.h>
#include <fcntl.h>
#include <ifaddrs.h>
#else
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#endif

#include <fftw3.h>
#include <sys/timeb.h>
#include <sys/timeb.h>
#include <pthread.h>
#include <stdbool.h>
#include <semaphore.h>
#include <signal.h>
#include <math.h>
#include <limits.h>
#include <string.h>
#include <libgen.h>
#include <errno.h>
#include <rtl-sdr.h>

#ifdef INCLUDE_NEON
#include <arm_neon.h>
#elif defined INCLUDE_SSE2
#include <xmmintrin.h>
#endif

#define PRG_VERSION "2.0" // see ChangeLog for history

#define HERMES_FW_VER 32
#define PORT 1024
#define MAX_BUFFER_LEN 2048
#define HPSDR_FRAME_LEN 1032
#define IQ_FRAME_DATA_LEN 63
#define RTL_BANDWIDTH 300000   // Tuner set to 400 Khz IF BW 
#define DOWNSAMPLE_192 8    // downsample value used to get 192khz
#define RTL_SAMPLE_RATE (192000 * DOWNSAMPLE_192)
#define RTL_READ_COUNT (16384 * DOWNSAMPLE_192)
#define MAX_RCVRS 7
#define IQ_FRAME_DATA_LEN 63
#define MAXSTR 128
#define FFT_SIZE 65536
#define CAL_STATE_EXIT  MAX_RCVRS
#define CAL_STATE_0     -1
#define CAL_STATE_1     -2
#define CAL_STATE_2     -3
#define CAL_STATE_3     -4

#define RTL_MODE_SKIMMER 0
#define RTL_MODE_WSPR    1

typedef struct {
    char     chunk_id[4];
    uint32_t chunk_size;
    char     format[4];
    char     fmtchunk_id[4];
    uint32_t fmtchunk_size;
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bps;
    char     datachunk_id[4];
    uint32_t datachunk_size;
} WavHeader;

struct main_cb {
        int total_num_rcvrs;
        int active_num_rcvrs;
        u_int rcvrs_mask;
        int nsamps_packet;
        int frame_offset1;
        int frame_offset2;
        int output_rate;
        int up_xtal;
        char sound_dev[MAXSTR];
        char ip_addr[MAXSTR];
        char serialstr[MAXSTR];

        // the last array member is used to remember last settings
        int agc_mode[MAX_RCVRS + 1];
        int bias_t[MAX_RCVRS + 1];
        int direct_mode[MAX_RCVRS + 1];
        int gain[MAX_RCVRS + 1];
        int freq_offset[MAX_RCVRS + 1];
        int center_freq[MAX_RCVRS + 1];
        int if_bw[MAX_RCVRS + 1];
        int gain_mode[MAX_RCVRS + 1];

        // Added to handle dynamic config file updates
        int last_agc_mode[MAX_RCVRS + 1];
        int last_bias_t[MAX_RCVRS + 1];
        int last_direct_mode[MAX_RCVRS + 1];
        int last_gain[MAX_RCVRS + 1];
        int last_freq_offset[MAX_RCVRS + 1];
        int last_center_freq[MAX_RCVRS + 1];
        int last_if_bw[MAX_RCVRS + 1];
        int last_gain_mode[MAX_RCVRS + 1];


        int rcvr_order[MAX_RCVRS + 1];
        int signal_multiplier;
        int cal_state;
        int calibrate;
        int rtl_mode;

        struct timeb freq_ltime[MAX_RCVRS];
        struct timeb freq_ttime[MAX_RCVRS];

        fftw_complex fftIn[sizeof(fftw_complex) * FFT_SIZE];
        fftw_complex fftOut[sizeof(fftw_complex) * FFT_SIZE];
        fftw_plan fftPlan;
        float fft_averaged[FFT_SIZE * sizeof(float)];

        struct rcvr_cb {
                float dest[4] __attribute__((aligned(16)));
                int rcvr_num;
                int new_freq;
                int curr_freq;
                int output_rate;
                u_int rcvr_mask;
                rtlsdr_dev_t* rtldev;
                struct main_cb* mcb;
                pthread_t hpsdrsim_sendiq_thr;
                pthread_t rtl_read_thr;

                int iqSample_offset;
                int iqSamples_remaining;
                float iqSamples[(RTL_READ_COUNT + (IQ_FRAME_DATA_LEN * 2))];

                float rtl_buf[RTL_READ_COUNT];
                float* iq_buf;
                char filename[MAX_BUFFER_LEN];
        } rcb[MAX_RCVRS];
};

void downsample(struct rcvr_cb* rcb);
void format_payload(void);
int init_rtl(int rcvr_num, int dev_index);
void load_packet(struct rcvr_cb* rcb);
void rtl_sighandler(int signum);
int parse_config(char* conf_file);

void* hpsdrsim_sendiq_thr_func(void* arg);
void* rtl_read_thr_func(void* arg);
void hpsdrsim_stop_threads();

#ifndef _WIN32
void open_local_sound(char* adevice);
void close_local_sound();
void write_local_sound(unsigned char* samples);
void reopen_local_sound();
#endif

#define ODEV_NONE          999
#define ODEV_METIS           0
#define ODEV_HERMES          1

#define NDEV_NONE          999
#define NDEV_ATLAS           0
#define NDEV_HERMES          1

//
// Forward declarations for new protocol stuff
//
void   new_protocol_general_packet(unsigned char *buffer);
int    new_protocol_running(void);

// using clock_nanosleep of librt
extern int clock_nanosleep(clockid_t __clock_id, int __flags,
                           __const struct timespec *__req,
                           struct timespec *__rem);

//
// message printing
//
#include <stdarg.h>
void t_print(const char *format, ...);
void t_perror(const char *string);

#endif // _RTL_HPSDR_H
