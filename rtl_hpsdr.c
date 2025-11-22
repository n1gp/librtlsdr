/* Copyright (C)
*   10/2025 - Rick Koch, N1GP
*   Wrote rtl_hpsdr using various open sources on the internet.
*     Christoph, https://github.com/dl1ycf/pihpsdr
*     https://github.com/steve-m/librtlsdr
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

/*
 * This program simulates an HPSDR Hermes board using USB data from
 * an RTL-SDR dongle.
 */

#include "rtl_hpsdr.h"

int fd;
int do_exit = 0;
unsigned char broadcastReply[60];
uint32_t last_sequence_number = 0;
uint32_t last_seqnum = 0, seqnum;
u_char last_rate[MAX_RCVRS] = { 0 };
unsigned int pid = 0;
struct main_cb mcb;
static float dcIlast[MAX_RCVRS] = { 0.0f }, dcQlast[MAX_RCVRS] = { 0.0f };

static int sock_udp;

static pthread_mutex_t iqready_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t iqready_cond = PTHREAD_COND_INITIALIZER;
static u_int rcvr_flags = 0;
pthread_mutex_t done_send_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t done_send_cond = PTHREAD_COND_INITIALIZER;
u_int send_flags = 0;
pthread_mutex_t send_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t send_cond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t liquid_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t liquid_cond = PTHREAD_COND_INITIALIZER;
u_char liquid_flags = 0;

static pthread_t rtl_read_thr[MAX_RCVRS];
static pthread_t hpsdrsim_sendiq_thr[MAX_RCVRS];

int running = 0;

static u_char buffer[MAX_BUFFER_LEN];
static float rtl_lut[6][256];

struct sockaddr_in addr_new;

// using clock_nanosleep of librt
extern int clock_nanosleep(clockid_t __clock_id, int __flags,
                           __const struct timespec *__req,
                           struct timespec *__rem);

uint64_t get_posix_clock_time_us()
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return (uint64_t)(ts.tv_sec * 1000000 + ts.tv_nsec / 1000);
    } else {
        return 0; // Error handling
    }
}

void *__memalign(size_t align, size_t len)
{
// below works but has coding issues
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"

    unsigned char *mem, *new, *end;
    size_t header, footer;

    if ((align & -align) != align) {
        errno = EINVAL;
        return NULL;
    }

    if (len > SIZE_MAX - align) {
        errno = ENOMEM;
        return NULL;
    }

    if (align <= 4*sizeof(size_t)) {
        if (!(mem = malloc(len)))
            return NULL;
        return mem;
    }

    if (!(mem = malloc(len + align-1)))
        return NULL;

    new = (void *)((uintptr_t)(mem + (align-1)) & -align);
    if (new == mem) return mem;

    header = ((size_t *)mem)[-1];

    if (!(header & 7)) {
        ((size_t *)new)[-2] = ((size_t *)mem)[-2] + (new-mem);
        ((size_t *)new)[-1] = ((size_t *)mem)[-1] - (new-mem);
        return new;
    }

    end = mem + (header & -8);
    footer = ((size_t *)end)[-2];

    ((size_t *)mem)[-1] = (header&7) | (new-mem);
    ((size_t *)new)[-2] = (footer&7) | (new-mem);
    ((size_t *)new)[-1] = (header&7) | (end-new);
    ((size_t *)end)[-2] = (footer&7) | (end-new);
#pragma GCC diagnostic pop
    free(mem);
    return new;
}

int my_posix_memalign(void **res, size_t align, size_t len)
{
    if (align < sizeof(void *)) return EINVAL;
    void *mem = __memalign(align, len);
    if (!mem) return errno;
    *res = mem;
    return 0;
}

int
float_malloc_align (void **voidptr, int alignment, int bytes)
{
    return (my_posix_memalign (voidptr, alignment, bytes));
}

void
float_free_align (float *ptr)
{
    free ((void *) ptr);
}

void rtlsdr_callback (unsigned char *buf, uint32_t len, void *ctx)
{
    int i, j;
    struct rcvr_cb *rcb = (struct rcvr_cb *) ctx;
    float dcI, dcQ, R = 0.999995;

    if (!rcb->enabled || do_exit || !running) {
        return;
    }

    if (RTL_READ_COUNT != len) {
        perror ("rtlsdr_callback(): RTL_READ_COUNT != len!\n");
        return;
    }

    // Convert to float and copy data to buffer, offset by coefficient length * 2.
    // The downsample routine will move the previous last coefficient length * 2
    // to the beginning of the buffer. This is because of the FIR filter length, the
    // filtering routine takes in 'filter_length' more samples than it outputs or
    // coefficient length * 2 for I&Q (stereo) input samples.
    for (i = 0, j = 0; i < RTL_READ_COUNT; j++, i += 2) {
#if 1   // remove DC component
        dcI = rtl_lut[last_rate[rcb->rcvr_num]][buf[i]] + dcIlast[rcb->rcvr_num] * R;
        dcQ = rtl_lut[last_rate[rcb->rcvr_num]][buf[i+1]] + dcQlast[rcb->rcvr_num] * R;
        rcb->iq_buf[j] = (dcI - dcIlast[rcb->rcvr_num]) +
                         ((dcQ - dcQlast[rcb->rcvr_num]) * _Complex_I);
        dcIlast[rcb->rcvr_num] = dcI;
        dcQlast[rcb->rcvr_num] = dcQ;
#else
        rcb->iq_buf[j] = buf[i+1] * 1000.0f + (buf[i]* 2500.0f * _Complex_I);
        //printf("I:%x Q:%x ", buf[i+1], buf[i]);
        //rcb->iq_buf[j] = buf[i+1] + (buf[i] * _Complex_I);
#endif
    }

    // below pthread_mutex_lock may have to be before loop above? TODO
    pthread_mutex_lock (&iqready_lock);
    rcvr_flags |= rcb->rcvr_mask;
    pthread_cond_broadcast (&iqready_cond);
    pthread_mutex_unlock (&iqready_lock);
}

void * rtl_read_thr_func (void *arg)
{
    struct rcvr_cb *rcb = (struct rcvr_cb *) arg;
    int i = rcb->rcvr_num;

    t_print("Starting rtl_read_thr_func() rcvr %d\n", i+1);
    rtlsdr_read_async (rcb->rtldev, rtlsdr_callback,
                       (void *) (&mcb.rcb[i]), 2, RTL_READ_COUNT);
    t_print("Ending rtl_read_thr_func() rcvr %d\n", i+1);
    pthread_exit (NULL);
}

void sdr_sighandler (int signum)
{
    t_print ("Signal caught, exiting!\n");
    do_exit = 1;
    running = 0;
    //hpsdrsim_stop_threads ();
}

int get_nearest_tuner_gain(rtlsdr_dev_t *dev, int target_gain)
{
        int i, r, err1, err2, count, nearest;
        int* gains;
        r = rtlsdr_set_tuner_gain_mode(dev, 1); 
        if (r < 0) {
                fprintf(stderr, "WARNING: Failed to enable manual gain.\n");
                return r;
        }
        count = rtlsdr_get_tuner_gains(dev, NULL);
        if (count <= 0) {
                return 0;
        }
        gains = malloc(sizeof(int) * count);
        count = rtlsdr_get_tuner_gains(dev, gains);
        nearest = gains[0];
        for (i=0; i<count; i++) {
                err1 = abs(target_gain - nearest);
                err2 = abs(target_gain - gains[i]);
                if (err2 < err1) {
                        nearest = gains[i];
                }
        }
        free(gains);
        return nearest;
}

int init_rtl (int rcvr_num, int dev_index)
{
    int i, r;
    rtlsdr_dev_t *rtldev;

    r = rtlsdr_open (&(mcb.rcb[rcvr_num].rtldev), dev_index);

    if (r < 0) {
        t_print ("ERROR:  Failed to open rtlsdr device\n");
        return (-1);
    }

    rtldev = mcb.rcb[rcvr_num].rtldev;

    if (mcb.if_bw[rcvr_num] == 0) {
        t_print ("Setting tuner IF BW to %d Hz\n", RTL_BANDWIDTH);
        r = rtlsdr_set_tuner_bandwidth (rtldev, RTL_BANDWIDTH);
        if (r < 0) {
            t_print ("WARNING: Failed to set IF BW to %d Hz!\n", RTL_BANDWIDTH);
            return (-1);
        }

        mcb.if_bw[rcvr_num] = RTL_BANDWIDTH;
    } else {
        t_print ("Setting tuner IF BW to %d Hz\n", mcb.if_bw[rcvr_num]);
        r = rtlsdr_set_tuner_bandwidth (rtldev, mcb.if_bw[rcvr_num]);
        if (r < 0) {
            t_print ("WARNING: Failed to set IF BW to %d Hz!\n",mcb.if_bw[rcvr_num]);
            return (-1);
        }
    }

    r = rtlsdr_set_sample_rate (rtldev, RTL_SAMPLE_RATE);
    if (r < 0) {
        t_print ("WARNING: Failed to set sample rate to %d!\n", RTL_SAMPLE_RATE);
        return (-1);
    }


#if 0 // maybe do something based on tuner type?
int tuner_type = rtlsdr_get_tuner_type(rtldev);
printf("RRK, tuner_type = %d\n", tuner_type);

from rtl-sdr.h
enum rtlsdr_tuner {
        RTLSDR_TUNER_UNKNOWN = 0,
        RTLSDR_TUNER_E4000,
        RTLSDR_TUNER_FC0012,
        RTLSDR_TUNER_FC0013,
        RTLSDR_TUNER_FC2580,
        RTLSDR_TUNER_R820T,
        RTLSDR_TUNER_R828D
};
#endif

    rtlsdr_set_center_freq (rtldev, 110000000 + mcb.up_freq);
    if (r < 0)
        t_print ("WARNING: Failed to set tuner freq!\n");

    r = rtlsdr_set_direct_sampling (rtldev, mcb.direct_mode);
    if (r < 0) {
        t_print ("WARNING: Failed to set direct sampling!\n");
        return (-1);
    } else
        t_print ("  direct sampling\t%d\n", mcb.direct_mode);

    //r = rtlsdr_set_agc_mode (rtldev, mcb.agc_mode[rcvr_num]);
    r = rtlsdr_set_agc_mode (rtldev, mcb.agc_mode);
    if (r < 0) {
        t_print ("WARNING: Failed to set automatic gain!\n");
        return (-1);
    } else
        t_print ("  auto agc mode\t%s\n",
                 (mcb.agc_mode) ? "on" : "off");

    if (0 == mcb.gain) {
        r = rtlsdr_set_tuner_gain_mode(rtldev, 0); 
        if (r < 0)
            t_print("WARNING: Failed to enable automatic gain.\n");
        else
            t_print("  tuner gain\t\tautomatic\n");
    } else {
        r = rtlsdr_set_tuner_gain_mode(rtldev, 1); 
        if (r < 0)
            t_print("WARNING: Failed to enable manual gain.\n");

        i = get_nearest_tuner_gain(rtldev, mcb.gain);
        r = rtlsdr_set_tuner_gain(rtldev, i);
        if (r < 0)
            t_print("WARNING: Failed to set tuner gain.\n");
        else
            t_print("  tuner gain\t\t%g dB\n", i/10.0);
    }
    r = rtlsdr_set_bias_tee (rtldev, mcb.bias_t[rcvr_num]);
    if (r < 0) {
        t_print ("WARNING: Failed to set bias T!\n");
        return (-1);
    } else
        t_print ("  bias_t\t\t%s\n\n",
                 (mcb.bias_t[rcvr_num]) ? "on" : "off");

    r = rtlsdr_reset_buffer (rtldev);
    if (r < 0) {
        t_print ("WARNING: Failed to reset buffers!\n");
        return (-1);
    }

    return (0);
}

char *time_stamp ()
{
    char *timestamp = (char *) malloc (sizeof (char) * 16);
    time_t ltime = time (NULL);
    struct tm *tm;

    tm = localtime (&ltime);
    sprintf (timestamp, "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec);
    return timestamp;
}

void * hpsdrsim_sendiq_thr_func (void *arg)
{
    int rtl_samples = 256, samps_packet = 238;
    struct rcvr_cb *rcb = (struct rcvr_cb *) arg;
    double now, last = 0;

    rcb->iqSample_offset = rcb->iqSamples_remaining = 0;

    t_print("Starting hpsdrsim_sendiq_thr_func() rcvr %d...\n", rcb->rcvr_num+1);
    while (!do_exit) {
        pthread_mutex_lock (&iqready_lock);
        while (!(rcvr_flags & rcb->rcvr_mask)) {
            pthread_cond_wait (&iqready_cond, &iqready_lock);
        }
        rcvr_flags &= ~rcb->rcvr_mask;
        pthread_mutex_unlock (&iqready_lock);

        // can happen when switching between rcvr numbers
        if (rcb->iqSamples_remaining < 0)
            rcb->iqSamples_remaining = 0;

        rtl_samples = rcb->output_rate / 750; // tied to rtl sample rate of 1536000

        // resample starting at any remaining offset
        pthread_mutex_lock (&liquid_lock);
        liquid_flags |= rcb->rcvr_mask;
        if(rcb->output_rate == 1536000) {
            memcpy(&rcb->iqSamples[rcb->iqSamples_remaining], &rcb->iq_buf[0], RTL_READ_COUNT*4);
        } else {
            firdecim_crcf_execute_block(rcb->decim, &rcb->iq_buf[0],
                                        rtl_samples, &rcb->iqSamples[rcb->iqSamples_remaining]);
        }
        liquid_flags &= ~(rcb->rcvr_mask);
        pthread_cond_broadcast (&liquid_cond);
        pthread_mutex_unlock (&liquid_lock);

        rcb->iqSamples_remaining += rtl_samples;

        while (rcb->iqSamples_remaining > samps_packet) {
            load_packet(rcb);
            rcb->iqSamples_remaining -= samps_packet;
            rcb->iqSample_offset += samps_packet;
        }

        // move remaining samples to beginning of buffer
        if ((rcb->iqSample_offset > 0) && (rcb->iqSamples_remaining > 0)) {
            memcpy (&(rcb->iqSamples[0]),
                    &(rcb->iqSamples[rcb->iqSample_offset]),
                    rcb->iqSamples_remaining * sizeof (float complex));
            rcb->iqSample_offset = 0;
        }

        if (rcb->new_freq == 162525) {rtlsdr_set_center_freq (rcb->rtldev, 162525000);rcb->new_freq = 0;} //RRK
        else if (rcb->new_freq) {
            clock_gettime(CLOCK_REALTIME, &mcb.freq_ttime[rcb->rcvr_num]);
            now = mcb.freq_ttime[rcb->rcvr_num].tv_sec + 1.0E-9 * mcb.freq_ttime[rcb->rcvr_num].tv_nsec;
            last = mcb.freq_ltime[rcb->rcvr_num].tv_sec + 1.0E-9 * mcb.freq_ltime[rcb->rcvr_num].tv_nsec;
            if ((now - last) > 0.2f) { // limit freq change to once per 200ms
                int i = rcb->new_freq + mcb.freq_offset[rcb->rcvr_num] + mcb.up_freq;
                rtlsdr_set_center_freq (rcb->rtldev, i);
                if (i < 0) {
                    t_print ("WARNING: Failed to set rcvr %d to freq %d with offset %+d\n",
                             rcb->rcvr_num + 1, rcb->new_freq, mcb.freq_offset[rcb->rcvr_num]);
                }

                rcb->curr_freq = rcb->new_freq;
                rcb->new_freq = 0;
                memcpy(&mcb.freq_ltime, &mcb.freq_ttime, sizeof(mcb.freq_ttime));
            }
        }
    }

    pthread_exit (NULL);
    t_print("Ending hpsdrsim_sendiq_thr_func() rcvr_mask%d...\n", rcb->rcvr_mask);
}

void hpsdrsim_stop_threads ()
{
    running = 0;
#if 0   //Segmentation fault?
    for (i = 0; i < mcb.total_num_rcvrs; i++) {
        pthread_cancel (mcb.rcb[i].hpsdrsim_sendiq_thr);
    }

    pthread_mutex_lock (&send_lock);
    send_flags = mcb.rcvrs_mask;
    pthread_cond_broadcast (&send_cond);
    pthread_mutex_unlock (&send_lock);

    pthread_mutex_lock (&iqready_lock);
    rcvr_flags = mcb.rcvrs_mask;
    pthread_cond_broadcast (&iqready_cond);
    pthread_mutex_unlock (&iqready_lock);
#endif
}

int main (int argc, char *argv[])
{
    int r, count = 0;
    uint8_t id[4] = { 0xef, 0xfe, 1, 6 };
    struct sockaddr_in addr_udp;
    struct sockaddr_in addr_from;
    socklen_t lenaddr;
    struct timeval tv;
    int yes = 1;
    int bytes_read;
    uint32_t i, code;
    uint32_t *code0 = (uint32_t *) buffer;  // fast access to code of first buffer
    const int MAC1 = 0x00;
    const int MAC2 = 0x1C;
    const int MAC3 = 0xC0;
    const int MAC4 = 0xA2;
    int MAC5 = 0x10;
    const int MAC6N = 0xDD; // P2
    int CmdOption;
    struct sigaction sigact;

    memset(&mcb, 0, sizeof(mcb));

    unsigned int M = 8;
    unsigned int m = 8;
    float As = 60;

    mcb.agc_mode = 0;
    mcb.gain = 0;
    mcb.direct_mode = 0;

    // Initialize per receiver config settings
    for (i = 0; i < MAX_RCVRS; i++) {
        mcb.bias_t[i] = 0;
        mcb.freq_offset[i] = 0;
        mcb.use_up[i] = 0;
        mcb.rcvr_order[i] = i + 1;
        mcb.rcb[i].output_rate = 192000;
        memset (&mcb.freq_ltime[i], 0, sizeof (mcb.freq_ltime[i]));
        mcb.rcb[i].decim = firdecim_crcf_create_kaiser(M, m, As);
        firdecim_crcf_set_scale(mcb.rcb[i].decim, 1.0f/(float)M);
    }

    while((CmdOption = getopt(argc, argv, ":a:C:d:g:r:u:h")) != -1) {
        switch(CmdOption) {
        case 'h':
            printf("Usage: %s <optional arguments>\n", basename(argv[0]));
            printf("optional arguments:\n");
            printf("-a set agc_mode (1-on 0-off))\n");
            printf("-C freq in hz in which to calibrate dongles\n");
            printf("-d direct sampling mode 0|1|2|3 (defaults 0 or auto, 1=I 2=Q 3=NOMOD\n");
            printf("-g tuner gain in 1/10's dB (0-auto))\n");
            printf("-h help (prints this usage)\n");
            printf("-r number of rcvrs to use (defaults to all detected)\n");
            printf("-u up converter frequency in Hz\n");
            return EXIT_SUCCESS;
            break;

        case 'a':
            mcb.agc_mode = atoi (optarg);
            break;
        case 'C':
            mcb.calibrate = atoi (optarg);
            break;
        case 'd':
            mcb.direct_mode = atoi (optarg);
            break;
        case 'g':
            mcb.gain = atoi (optarg);
            break;
        case 'r':
            mcb.total_num_rcvrs = atoi (optarg);
            break;
        case 'u':
            mcb.up_freq = atoi (optarg);
            break;
        }
    }
    printf("\n");

    r = rtlsdr_get_device_count ();
    if (r) {
        t_print ("Found %d RTL device(s)", r);

        if ((mcb.total_num_rcvrs > r) || !mcb.total_num_rcvrs)
            mcb.total_num_rcvrs = r;

        if (mcb.total_num_rcvrs > MAX_RCVRS)
            mcb.total_num_rcvrs = MAX_RCVRS;

        t_print (", using %d.\n", mcb.total_num_rcvrs);
    } else {
        t_print ("No RTL devices found, exiting.\n");
        exit (0);
    }

    sigact.sa_handler = sdr_sighandler;
    sigemptyset (&sigact.sa_mask);
    sigact.sa_flags = 0;
    sigaction (SIGINT, &sigact, NULL);
    sigaction (SIGTERM, &sigact, NULL);
    sigaction (SIGQUIT, &sigact, NULL);
    sigaction (SIGPIPE, &sigact, NULL);

    pthread_mutex_init (&iqready_lock, NULL);
    pthread_cond_init (&iqready_cond, NULL);
    pthread_mutex_init (&send_lock, NULL);
    pthread_cond_init (&send_cond, NULL);
    pthread_mutex_init (&done_send_lock, NULL);
    pthread_cond_init (&done_send_cond, NULL);

    // create a lookup table for float values for the 6 sample rates
    // increase amplitude for lower bands due to increased decimation
    for (r = 0; r < 6; r++) {
        for (i = 0; i < 256; i++) {
            rtl_lut[r][i] = (float)((i - 127.4f) / 128.0f) * 200 * (256/(r+1));
        }
    }

    for (i = 0; i < mcb.total_num_rcvrs; i++) {
        mcb.rcb[i].mcb = &mcb;
        mcb.rcb[i].new_freq = 0;
        mcb.rcb[i].output_rate = 192000;

        mcb.rcb[i].hpsdrsim_sendiq_thr = hpsdrsim_sendiq_thr[i];
        mcb.rcb[i].rcvr_num = i;
        mcb.rcvrs_mask |= 1 << i;
        mcb.rcb[i].rcvr_mask = 1 << i;

        if (0 != init_rtl (i, mcb.rcvr_order[i] - 1)) {
            t_print ("ERROR: Failed init_rtl rcvr%d hardware!\n", i + 1);
            return (-1);
        }

        r = float_malloc_align ((void **) &(mcb.rcb[i].iq_buf), 16,
                                (RTL_READ_COUNT * sizeof (float complex)));
        if (r != 0) {
            t_print ("failed to allocate iq_buf aligned memory: r=%d\n", r);
            return (r);
        }

        if ((r = pthread_create (&rtl_read_thr[i], NULL, rtl_read_thr_func,
                                 &mcb.rcb[i]))) {
            t_print ("error: pthread_create, r: %d\n", r);
            return (r);
        }

        if ((r = pthread_create (&hpsdrsim_sendiq_thr[i], NULL,
                                 hpsdrsim_sendiq_thr_func, &mcb.rcb[i]))) {
            t_print ("pthread_create failed on hpsdrsim_sendiq_thr: r=%d\n", r);
            return (r);
        }
    }

    if ((sock_udp = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        t_perror("socket");
        return EXIT_FAILURE;
    }

    setsockopt(sock_udp, SOL_SOCKET, SO_REUSEADDR, (void *)&yes, sizeof(yes));
    setsockopt(sock_udp, SOL_SOCKET, SO_REUSEPORT, (void *)&yes, sizeof(yes));
    tv.tv_sec = 0;
    tv.tv_usec = 1000;
    setsockopt(sock_udp, SOL_SOCKET, SO_RCVTIMEO, (void *)&tv, sizeof(tv));
    memset(&addr_udp, 0, sizeof(addr_udp));
    addr_udp.sin_family = AF_INET;
    addr_udp.sin_addr.s_addr = htonl(INADDR_ANY);
    addr_udp.sin_port = htons(1024);

    if (bind(sock_udp, (struct sockaddr *)&addr_udp, sizeof(addr_udp)) < 0) {
        t_perror("main ERROR: bind");
        return EXIT_FAILURE;
    }
    t_print("Waiting on Discovery...\n");

    while (!do_exit) {
        memcpy(buffer, id, 4);
        count++;
        lenaddr = sizeof(addr_from);
        bytes_read = recvfrom(sock_udp, buffer, HPSDR_FRAME_LEN, 0, (struct sockaddr *)&addr_from, &lenaddr);

        if (bytes_read < 0 && errno != EAGAIN) {
            t_perror("recvfrom");
            continue;
        }

        if (bytes_read <= 0) {
            continue;
        }

        count = 0;
        code = *code0;

        /*
         * Here we have to handle the following "non standard" cases:
         * NewProtocol "Discovery" packet   60 bytes starting with 00 00 00 00 02
         * NewProtocol "General"   packet   60 bytes starting with 00 00 00 00 00
         *                                  ==> this starts NewProtocol radio
         */
        if (code == 0 && buffer[4] == 0x02) {
            t_print("NewProtocol discovery packet received from %s\n", inet_ntoa(addr_from.sin_addr));
            // prepare response
            memset(buffer, 0, 60);
            buffer [4] = 0x02 + new_protocol_running();
            buffer [5] = MAC1;
            buffer[ 6] = MAC2;
            buffer[ 7] = MAC3;
            buffer[ 8] = MAC4;
            buffer[ 9] = MAC5; // specifies type of radio
            buffer[10] = MAC6N; // encodes new protocol
            buffer[11] = NDEV_HERMES;
            buffer[12] = 38;
            buffer[13] = 18;
            buffer[20] = mcb.total_num_rcvrs;
            buffer[21] = 1;
            buffer[22] = 3;

            sendto(sock_udp, buffer, 60, 0, (struct sockaddr *)&addr_from, sizeof(addr_from));
            continue;
        }

        if (bytes_read == 60 && buffer[4] == 0x00) {
            // handle "general packet" of the new protocol
            memset(&addr_new, 0, sizeof(addr_new));
            addr_new.sin_family = AF_INET;
            addr_new.sin_addr.s_addr = addr_from.sin_addr.s_addr;
            addr_new.sin_port = addr_from.sin_port;
            new_protocol_general_packet(buffer);
            continue;
        }
    }

    close(sock_udp);

    return EXIT_SUCCESS;
}

void t_print(const char *format, ...)
{
    va_list(args);
    va_start(args, format);
    struct timespec ts;
    double now;
    static double starttime;
    static int first = 1;
    char line[1024];
    clock_gettime(CLOCK_MONOTONIC, &ts);
    now = ts.tv_sec + 1E-9 * ts.tv_nsec;

    if (first) {
        first = 0;
        starttime = now;
    }

    //
    // After 11 days, the time reaches 999999.999 so we simply wrap around
    //
    if (now - starttime >= 999999.995) {
        starttime += 1000000.0;
    }

    //
    // We have to use vsnt_print to handle the varargs stuff
    // g_print() seems to be thread-safe but call it only ONCE.
    //
    vsnprintf(line, 1024, format, args);
    printf("%10.6f %s", now - starttime, line);
}

void t_perror(const char *string)
{
    t_print("%s: %s\n", string, strerror(errno));
}

// protocol2 stuff

static int bits = -1;
static long rxfreq[MAX_RCVRS];
static int ddcenable[MAX_RCVRS];
static int rxrate[MAX_RCVRS];
static int stepatt0 = -1;
static int ddc_port = 1025;
static int mic_port = 1026;
static int hp_port = 1027;
static int ddc0_port = 1035;
static unsigned char pbuf[238*6];

static pthread_t highprio_thread_id = 0;
static pthread_t ddc_specific_thread_id = 0;
static pthread_t mic_thread_id;
static pthread_t rx_thread_id[MAX_RCVRS];

void   *highprio_thread(void*);
void   *ddc_specific_thread(void*);
void   *mic_thread(void *);
void   *rx_thread(void *);

int new_protocol_running()
{
    if (running) {
        return 1;
    } else {
        return 0;
    }
}

void load_packet (struct rcvr_cb *rcb)
{
    float complex *out_buf = &rcb->iqSamples[rcb->iqSample_offset];
    int i, j, IQData;

    pthread_mutex_lock (&send_lock);
    for (i = 0, j = 0; i < 238; i++, j+=6) {
        IQData = (int)cimagf(out_buf[i]);
        pbuf[j] = IQData >> 16;
        pbuf[j+1] = IQData >> 8;
        pbuf[j+2] = IQData & 0xff;
        IQData = (int)crealf(out_buf[i]);
        pbuf[j+3] = IQData >> 16;
        pbuf[j+4] = IQData >> 8;
        pbuf[j+5] = IQData & 0xff;
    }

    send_flags |= rcb->rcvr_mask;
    pthread_cond_broadcast (&send_cond);
    pthread_mutex_unlock (&send_lock);

    pthread_mutex_lock (&done_send_lock);
    while (send_flags & rcb->rcvr_mask) {
        pthread_cond_wait (&done_send_cond, &done_send_lock);
    }
    pthread_mutex_unlock (&done_send_lock);
}

void new_protocol_general_packet(unsigned char *buffer)
{
    static unsigned long seqnum = 0;
    unsigned long seqold;
    seqold = seqnum;
    seqnum = (buffer[0] >> 24) + (buffer[1] << 16) + (buffer[2] << 8) + buffer[3];

    if ((seqnum != 0 && seqnum != seqold + 1 ) && seqold != 0) {
        t_print("GP: SEQ ERROR, old=%lu new=%lu\n", seqold, seqnum);
    }

    //
    // Start HighPrio thread if we arrive here for the first time
    // The HighPrio thread keeps running all the time.
    //
    if (!highprio_thread_id) {
        if (pthread_create(&highprio_thread_id, NULL, highprio_thread, NULL) < 0) {
            t_perror("***** ERROR: Create HighPrio thread");
        }
        pthread_detach(highprio_thread_id);

        if (!ddc_specific_thread_id) {
            if (pthread_create(&ddc_specific_thread_id, NULL, ddc_specific_thread, NULL) < 0) {
                t_perror("***** ERROR: Create DDC specific thread");
            }
        }
    }
}

void *highprio_thread(void *data)
{
    int sock;
    struct sockaddr_in addr;
    socklen_t lenaddr = sizeof(addr);
    unsigned long seqnum = 0, seqold;
    unsigned char buffer[2000];
    struct timeval tv;
    int yes = 1;
    int rc;
    long freq;
    int i;
    sock = socket(AF_INET, SOCK_DGRAM, 0);

    if (sock < 0) {
        t_perror("***** ERROR: HP: socket");
        return NULL;
    }

    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (void *)&yes, sizeof(yes));
    setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, (void *)&yes, sizeof(yes));
    tv.tv_sec = 0;
    tv.tv_usec = 10000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (void *)&tv, sizeof(tv));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(hp_port);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        t_perror("highprio_thread ERROR: bind");
        close(sock);
        return NULL;
    }

    t_print("Starting highprio_thread()\n");
    while (1) {
        rc = recvfrom(sock, buffer, 1444, 0, (struct sockaddr *)&addr, &lenaddr);

        if (rc < 0 && errno != EAGAIN) {
            t_perror("***** ERROR: HighPrio thread: recvmsg");
            break;
        }

        if (rc < 0) {
            continue;
        }

        if (rc != 1444) {
            t_print("Received HighPrio packet with incorrect length %d\n", rc);
            break;
        }

        seqold = seqnum;
        seqnum = (buffer[0] >> 24) + (buffer[1] << 16) + (buffer[2] << 8) + buffer[3];

        if ((seqnum != 0 && seqnum != seqold + 1 ) && seqold != 0) {
            t_print("HP: SEQ ERROR, old=%lu new=%lu\n", seqold, seqnum);
        }

        for (i = 0; i < MAX_RCVRS; i++) {
            freq = (buffer[ 9 + 4 * i] << 24) + (buffer[10 + 4 * i] << 16) + (buffer[11 + 4 * i] << 8) + buffer[12 + 4 * i];

            if (bits & 0x08) {
                freq = round(122880000.0 * (double) freq / 4294967296.0);
            }

            if (freq != rxfreq[i]) {
                mcb.rcb[i].new_freq = rxfreq[i] = freq;
                //t_print("HP: DDC%d freq: %lu\n", i, freq);
                // freq set in hpsdrsim_sendiq_thr_func() after delay
            }
        }

        rc = buffer[1443];
        if (rc != stepatt0) {
            stepatt0 = rc;
            //t_print("HP: StepAtt0 = %d\n", stepatt0);
            if (stepatt0 != 0) {
                for (i = 0; i < mcb.total_num_rcvrs; i++) { // set all the same, for now
                    // convert attn to gain
                    rc = get_nearest_tuner_gain(mcb.rcb[i].rtldev, (31 - stepatt0) * 16);
                    mcb.gain = rc;
                    rc = rtlsdr_set_tuner_gain(mcb.rcb[i].rtldev, mcb.gain);
                    if (rc < 0)
                        t_print("WARNING: Failed to set tuner gain.\n");
                }
            }
        }

        rc = (buffer[4] >> 0) & 0x01;
        if (rc != running) {
            running = rc;
            t_print("HP: Running=%d\n", rc);

            // if running=0, wait for threads to complete, otherwise spawn them off
            if (running) {
                for (i = 0; i < mcb.total_num_rcvrs; i++) {
                    if (pthread_create(&rx_thread_id[i], NULL, rx_thread, (void *) (uintptr_t) i) < 0) {
                        t_perror("***** ERROR: Create RX thread");
                    }
                }

                if (pthread_create(&mic_thread_id, NULL, mic_thread, NULL) < 0) {
                    t_perror("***** ERROR: Create Mic thread");
                }
            } else {
                // Clean-Up done below
                break;
            }
        }
    }

    running = 0;

    for (i = 0; i < MAX_RCVRS; i++) {
        pthread_mutex_lock (&send_lock);
        pthread_cond_broadcast (&send_cond);
        pthread_mutex_unlock (&send_lock);
        rc = pthread_join(rx_thread_id[i], NULL);
        ddcenable[i] = 0;
    }

    highprio_thread_id = 0; // needs to be 0 before joining ddc_specific_thread_id

    rc = pthread_join(ddc_specific_thread_id, NULL);
    ddc_specific_thread_id = 0;
    rc = pthread_join(mic_thread_id, NULL);

    t_print("Ending highprio_thread()\n");
    close(sock);
    return NULL;
}

void *ddc_specific_thread(void *data)
{
    int sock;
    struct sockaddr_in addr;
    socklen_t lenaddr = sizeof(addr);
    unsigned long seqnum, seqold;
    struct timeval tv;
    unsigned char buffer[2000];
    int yes = 1;
    int rc;
    int M = 8, i;

    sock = socket(AF_INET, SOCK_DGRAM, 0);

    if (sock < 0) {
        t_perror("***** ERROR: ddc_specific_thread: socket");
        return NULL;
    }

    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (void *)&yes, sizeof(yes));
    setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, (void *)&yes, sizeof(yes));
    tv.tv_sec = 0;
    tv.tv_usec = 10000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (void *)&tv, sizeof(tv));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(ddc_port);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        t_perror("ddc_specific_thread ERROR: bind");
        close(sock);
        return NULL;
    }

    seqnum = 0;

    t_print("Starting ddc_specific_thread()\n");
    while (highprio_thread_id) {
        rc = recvfrom(sock, buffer, 1444, 0, (struct sockaddr *)&addr, &lenaddr);
        if (rc < 0 && errno != EAGAIN) {
            t_perror("***** ERROR: DDC specific thread: recvmsg");
            break;
        }

        if (rc < 0) {
            continue;
        }

        if (rc != 1444) {
            t_print("RXspec: Received DDC specific packet with incorrect length");
            break;
        }

        seqold = seqnum;
        seqnum = (buffer[0] >> 24) + (buffer[1] << 16) + (buffer[2] << 8) + buffer[3];

        if ((seqnum != 0 && seqnum != seqold + 1 ) && seqold != 0) {
            t_print("RXspec: SEQ ERROR, old=%lu new=%lu\n", seqold, seqnum);
        }

        for (i = 0; i < MAX_RCVRS; i++) {
            int modified = 0;

            rc = (buffer[18 + 6 * i] << 8) + buffer[19 + 6 * i];

            if (rc != rxrate[i] && rc != 0) {
                rxrate[i] = rc;
                modified = 1;

                switch(rxrate[i]) {
                case 48:
                    last_rate[i] = 0;
                    break;
                case 96:
                    last_rate[i] = 1;
                    break;
                case 192:
                    last_rate[i] = 2;
                    break;
                case 384:
                    last_rate[i] = 3;
                    break;
                case 768:
                    last_rate[i] = 4;
                    break;
                case 1536:
                    last_rate[i] = 5;
                }

                mcb.rcb[i].output_rate = rxrate[i] * 1000;

                if (rxrate[i] != 1536) { //RTL is sampling @ 1536k
                    M = 1536 / rxrate[i];

                    pthread_mutex_lock (&liquid_lock);
                    while (liquid_flags & mcb.rcb[i].rcvr_mask) {
                        if (!running) break;
                        pthread_cond_wait (&liquid_cond, &liquid_lock);
                    }

                    firdecim_crcf_destroy(mcb.rcb[i].decim);
                    mcb.rcb[i].decim = firdecim_crcf_create_kaiser(M, 8, 60.0f);
                    firdecim_crcf_set_scale(mcb.rcb[i].decim, 1.0f/(float)M);

                    pthread_mutex_unlock (&liquid_lock);
                }
            }

            rc = (buffer[7 + (i / 8)] >> (i % 8)) & 0x01;

            if (rc != ddcenable[i]) {
                modified = 1;
                mcb.rcb[i].enabled = ddcenable[i] = rc;
            }

            if (modified) {
                t_print("RX: DDC%d Enable=%d Rate=%d\n",
                        i, ddcenable[i], rxrate[i]);
                rc = 0;
            }
        }
    }

    close(sock);
    t_print("Ending ddc_specific_thread()\n");
    return NULL;
}

void *rx_thread(void *data)
{
    int sock;
    struct sockaddr_in addr;
    // One instance of this thread is started for each DDC
    unsigned long seqnum;
    unsigned char buffer[1444];
    int myddc;
    int yes = 1;
    unsigned char *p;
    myddc = (int) (uintptr_t) data;
    struct rcvr_cb *rcb = &mcb.rcb[myddc];

    if (myddc < 0 || myddc >= MAX_RCVRS) {
        return NULL;
    }

    seqnum = 0;

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        t_perror("***** ERROR: RXthread: socket");
        return NULL;
    }

    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (void *)&yes, sizeof(yes));
    setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, (void *)&yes, sizeof(yes));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(ddc0_port + myddc);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        t_perror("rx_threadi ERROR: bind");
        close(sock);
        return NULL;
    }

    pthread_mutex_lock (&done_send_lock);
    send_flags |= rcb->rcvr_mask;
    pthread_cond_broadcast (&done_send_cond);
    pthread_mutex_unlock (&done_send_lock);

    t_print("Starting rx_thread (%d)\n", myddc);
    while (running) {
        if (ddcenable[myddc] <= 0 || rxrate[myddc] == 0 || rxfreq[myddc] == 0) {
            usleep(500000);
            continue;
        }

        p = buffer;
        *p++ = (seqnum >> 24) & 0xFF;
        *p++ = (seqnum >> 16) & 0xFF;
        *p++ = (seqnum >>  8) & 0xFF;
        *p++ = (seqnum >>  0) & 0xFF;
        seqnum += 1;

        // no time stamps
        p += 9;

        *p++ = 24; // 24 bits per sample
        *p++ = 0;
        *p++ = 238; // samps per packet

        pthread_mutex_lock (&send_lock);
        while (!(send_flags & rcb->rcvr_mask)) {
            //while (0) {
            if(running == 0) break;
            pthread_cond_wait (&send_cond, &send_lock);
        }
        pthread_mutex_unlock (&send_lock);

        memcpy(p, pbuf, 1428); // I-Q data

#if 0
        if (seqnum == 100) {
            t_print ("rcvrs_mask:%x send_flags:%d\n", mcb.rcvrs_mask, send_flags);

            for (int i = 0; i < 1444; i++) {
                printf("%4d:%2x ", i, buffer[i]);

                if (!((i + 1) % 8))
                    printf("\n");
            }
        }
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        double now = ts.tv_sec + 1E-9 * ts.tv_nsec;
        static double last;
        printf("%10.6f\n", now-last);
        last = now;
#endif

        if (sendto(sock, buffer, 1444, 0, (struct sockaddr * )&addr_new, sizeof(addr_new)) < 0) {
            t_perror("***** ERROR: RX thread sendto");
            break;
        }

        pthread_mutex_lock (&done_send_lock);
        send_flags &= ~(rcb->rcvr_mask);
        pthread_cond_broadcast (&done_send_cond);
        pthread_mutex_unlock (&done_send_lock);
    }

    close(sock);
    t_print("Ending rx_thread (%d)\n", myddc);
    return NULL;
}

//
// The microphone thread just sends silence, that is
// a "zeroed" mic frame every 1.333 msec
//
void *mic_thread(void *data)
{
    int sock;
    unsigned long seqnum = 0;
    struct sockaddr_in addr;
    unsigned char buffer[132];
    unsigned char *p;
    int yes = 1;
    struct timespec delay;
    sock = socket(AF_INET, SOCK_DGRAM, 0);

    if (sock < 0) {
        t_perror("***** ERROR: Mic thread: socket");
        return NULL;
    }

    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (void *)&yes, sizeof(yes));
    setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, (void *)&yes, sizeof(yes));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(mic_port);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        t_perror("mic_thread ERROR: bind");
        close(sock);
        return NULL;
    }

    memset(buffer, 0, 132);
    clock_gettime(CLOCK_MONOTONIC, &delay);

    t_print("Starting mic_thread\n");
    while (running) {
        // update seq number
        p = buffer;
        *p++ = (seqnum >> 24) & 0xFF;
        *p++ = (seqnum >> 16) & 0xFF;
        *p++ = (seqnum >>  8) & 0xFF;
        *p++ = (seqnum >>  0) & 0xFF;
        seqnum++;
        // 64 samples with 48000 kHz, makes 1333333 nsec
        delay.tv_nsec += 1333333;

        while (delay.tv_nsec >= 1000000000) {
            delay.tv_nsec -= 1000000000;
            delay.tv_sec++;
        }

        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &delay, NULL);

        if (sendto(sock, buffer, 132, 0, (struct sockaddr * )&addr_new, sizeof(addr_new)) < 0) {
            t_perror("***** ERROR: Mic thread sendto");
            break;
        }
    }

    t_print("Ending mic_thread\n");
    close(sock);
    return NULL;
}
