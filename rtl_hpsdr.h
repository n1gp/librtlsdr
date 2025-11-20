#ifndef _RFSPACE_HPSDR_H
#define _RFSPACE_HPSDR_H

/* Copyright (C)
*
*   10/2025 - Rick Koch, N1GP
*   Wrote rfspace_hpsdr using various open sources on the internet.
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

#include <sched.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <stdbool.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <termios.h>
#include <libgen.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netdb.h>
#include <complex.h>
#include <liquid/liquid.h>
#include <rtl-sdr.h>

#define PRG_VERSION "1.0" // see ChangeLog for history

#define HERMES_FW_VER 18
#define MAX_BUFFER_LEN 2048
#define HPSDR_FRAME_LEN 1032
#define IQ_FRAME_DATA_LEN 63
#define RTL_SAMPLE_RATE 1536000
#define RTL_READ_COUNT 4096
#define RTL_BANDWIDTH 300000   // Tuner set to 400 Khz IF BW
#define MAX_RCVRS 7
#define IQ_FRAME_DATA_LEN 63
#define MAXSTR 128

struct main_cb {
    int total_num_rcvrs;
    int active_num_rcvrs;
    u_int rcvrs_mask;
    int nsamps_packet;
    int up_freq;
    char sound_dev[MAXSTR];
    char serialstr[MAXSTR];

    // the last array member is used to remember last settings
    int agc_mode;
    int bias_t[MAX_RCVRS + 1];
    int direct_mode[MAX_RCVRS + 1];
    int gain;
    int calibrate;
    int freq_offset[MAX_RCVRS + 1];
    int center_freq[MAX_RCVRS + 1];
    int if_bw[MAX_RCVRS + 1];
    int rcvr_order[MAX_RCVRS + 1];
    int use_up[MAX_RCVRS + 1];

    struct timespec freq_ltime[MAX_RCVRS];
    struct timespec freq_ttime[MAX_RCVRS];

    struct rcvr_cb {
        int rcvr_num;
        int enabled;
        int new_freq;
        int curr_freq;
        int output_rate;
        u_int rcvr_mask;
        struct main_cb* mcb;
        pthread_t hpsdrsim_sendiq_thr;
        rtlsdr_dev_t* rtldev;

        int iqSample_offset;
        int iqSamples_remaining;
        float complex iqSamples[RTL_READ_COUNT + IQ_FRAME_DATA_LEN * 2];
        float complex *iq_buf;
        firdecim_crcf decim;
    } rcb[MAX_RCVRS];
};

void load_packet(struct rcvr_cb* rcb);
void sdr_sighandler(int signum);

void* hpsdrsim_sendiq_thr_func(void* arg);
void hpsdrsim_stop_threads();

#define ODEV_NONE          999
#define ODEV_HERMES          1
#define ODEV_ORION           5

#define NDEV_NONE          999
#define NDEV_HERMES          1
#define NDEV_ORION           4

//##################### NEW PROTOCOL STUFF
#define LENNOISE 1536000
#define NOISEDIV (RAND_MAX / 768000)
#define OLDRTXLEN 64512
#define NEWRTXLEN 64320
#define IM3a  0.60
#define IM3b  0.20

//
// Forward declarations for new protocol stuff
//
void new_protocol_general_packet(unsigned char *buffer);
int  new_protocol_running(void);
void *mic_thread(void *data);
void *rx_thread(void *data);
void *ddc_specific_thread(void *data);
void *highprio_thread(void *data);
void load_packet (struct rcvr_cb *rcb);

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

#endif // _RFSPACE_HPSDR_H
