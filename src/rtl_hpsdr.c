/* Copyright (C)
* 2019 - Christoph van Wüllen, DL1YCF
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

/*
 * This program simulates an HPSDR Hermes board.
 */
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <stdbool.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <termios.h>
#include <libgen.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/timeb.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <net/if.h>

#define EXTERN
#include "rtl_hpsdr.h"
#include "rtl-sdr.h"

/*
 * These variables store the state of the "old protocol" SDR.
 * Whenever they are changed, this is reported.
 */

static int              rate = -1;
static long             rx_freq[7] = {-1, -1, -1, -1, -1, -1, -1};
static int              freq = -1;

static int sock_udp;

static void process_ep2();
static void *handler_ep6(void *arg);

static int oldnew = 1;  // 1: only P1, 2: only P2, 3: P1 and P2,
static int receivers = -1;
static int active_thread = 0;
static int enable_thread = 0;

static struct timeb test_start_time;
static struct timeb test_end_time;

struct main_cb mcb;

static pthread_mutex_t iqready_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t iqready_cond = PTHREAD_COND_INITIALIZER;
static u_int rcvr_flags = 0;
static pthread_mutex_t send_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t send_cond = PTHREAD_COND_INITIALIZER;
static u_int send_flags = 0;
static pthread_mutex_t done_send_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t done_send_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t do_cal_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t do_cal_cond = PTHREAD_COND_INITIALIZER;

static pthread_t rtl_read_thr[MAX_RCVRS];
static pthread_t hpsdrsim_sendiq_thr[MAX_RCVRS];
static pthread_t do_cal_thr;

static int num_copy_rcvrs = 0, do_exit = 0;
static int copy_rcvr[MAX_RCVRS];
static u_char last_num_rcvrs = 0;
static u_char last_rate = 0;
static int cal_rcvr = -1;
static int cal_rcvr_mask = 0;
static int cal_count[MAX_RCVRS] = { 0 };
static int last_freq[MAX_RCVRS] = { 0 };
static float dcIlast[MAX_RCVRS] = { 0.0f }, dcQlast[MAX_RCVRS] = { 0.0f };

static int common_freq = 0;

static int reveal_socket;

static int nsamps_packet[8] = { 126, 72, 50, 38, 30, 26, 22, 20 };
static int frame_offset1[8] = { 520, 520, 516, 510, 496, 510, 500, 516 };
static int frame_offset2[8] = { 1032, 1032, 1028, 1022, 1008, 1022, 1012, 1028 };

static int running = 0;
static char conf_file[MAXSTR];

static u_char buffer[MAX_BUFFER_LEN];
static u_char payload[HPSDR_FRAME_LEN];

static u_int hpsdr_sequence = 0;
static u_int pc_sequence;
static u_int network_error = 0;

static float rtl_lut[4][256];
static u_char *fft_buf;
static int reset_cal = 0;
static int do_cal_time = 5;    //seconds between calibration
static bool using_IQfiles = false, using_IQrewind = false;

static struct sockaddr_in addr_new;
static struct sockaddr_in addr_old;

// using clock_nanosleep of librt
extern int clock_nanosleep(clockid_t __clock_id, int __flags,
                           __const struct timespec *__req,
                           struct timespec *__rem);

void
rtl_sighandler (int signum)
{
    t_print ("Signal caught, exiting!\n");
    do_exit = 1;
    if (running)
        running = 0;
    else
        mcb.calibrate = 0;
    //hpsdrsim_stop_threads (); //Segmentation fault?
}

// returns the current time.
char *
time_stamp ()
{
    char *timestamp = (char *) malloc (sizeof (char) * 16);
    time_t ltime = time (NULL);
    struct tm *tm;

    tm = localtime (&ltime);
    sprintf (timestamp, "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec);
    return timestamp;
}

int
float_malloc_align (void **voidptr, int alignment, int bytes)
{
    return (posix_memalign (voidptr, alignment, bytes));
}

void
float_free_align (float *ptr)
{
    free ((void *) ptr);
}

void
load_packet (struct rcvr_cb *rcb)
{
    int b, i, j, k, copy_total = (mcb.active_num_rcvrs - 1) + num_copy_rcvrs;

    // if we need to copy a receiver we'll choose the last active 'real' one
    bool do_copy = ((num_copy_rcvrs > 0)
                    && (rcb->rcvr_num == mcb.active_num_rcvrs - 1));
    int offset1 = (num_copy_rcvrs > 0) ? frame_offset1[copy_total] : mcb.frame_offset1;
    int offset2 = (num_copy_rcvrs > 0) ? frame_offset2[copy_total] : mcb.frame_offset2;
    int offsetx;
    float *out_buf = &rcb->iqSamples[rcb->iqSample_offset * 2];
    int IQData;

    i = 0;
    k = 0;
    b = 16;

    pthread_mutex_lock (&send_lock);

    // insert data in lower and upper bank for each of the receivers
    while (k++ < 2)
    {
        offsetx = (i == 0) ? offset1 : offset2;
        while (b < offsetx)
        {
            for (j = 0; j < mcb.active_num_rcvrs + num_copy_rcvrs; j++)
            {
                if ((j == rcb->rcvr_num) || (do_copy && copy_rcvr[j] == j))
                {
                    if (using_IQfiles)
                    {
                        IQData = ((int)(out_buf[i]));
                        payload[b++] = (IQData & 0xff00) >> 8;
                        payload[b++] = (IQData & 0xff0000) >> 16;
                        payload[b++] = (IQData & 0xff000000) >> 24;
                        IQData = ((int)(out_buf[i+1]));
                        payload[b++] = (IQData & 0xff00) >> 8;
                        payload[b++] = (IQData & 0xff0000) >> 16;
                        payload[b++] = (IQData & 0xff000000) >> 24;
                    }
                    else
                    {
                        IQData = (int) out_buf[i];
                        payload[b++] = IQData >> 8;
                        payload[b++] = IQData & 0xff;
                        b++;
                        IQData = (int) out_buf[i + 1];
                        payload[b++] = IQData >> 8;
                        payload[b++] = IQData & 0xff;
                        b++;
                    }

                    if (do_copy)
                    {
                        if (j == copy_total)
                            i += 2;
                    }
                    else
                    {
                        i += 2;
                    }
                }
                else
                {
                    b += 6;
                }
            }
            b += 2;                // skip mic data
        }
        b = 528;
    }

    send_flags |= rcb->rcvr_mask;
    pthread_cond_broadcast (&send_cond);
    pthread_mutex_unlock (&send_lock);

    pthread_mutex_lock (&done_send_lock);
    while (send_flags & rcb->rcvr_mask)
    {
        pthread_cond_wait (&done_send_cond, &done_send_lock);
    }
    pthread_mutex_unlock (&done_send_lock);
}

void *
hpsdrsim_sendiq_thr_func (void *arg)
{
    int samps_packet, i;
    char num[16];
    register int rxnum;

    struct rcvr_cb *rcb = (struct rcvr_cb *) arg;

    rcb->iqSample_offset = rcb->iqSamples_remaining = 0;

    //t_print("ENTERING hpsdrsim_sendiq_thr_func() rcvr %d...\n", rcb->rcvr_num+1);
    while (!do_exit)
    {
        if (!running) rcb->iqSample_offset = rcb->iqSamples_remaining = 0;
        samps_packet = (num_copy_rcvrs > 0)
                       ? nsamps_packet[(mcb.active_num_rcvrs - 1) + num_copy_rcvrs]
                       : mcb.nsamps_packet;

        pthread_mutex_lock (&iqready_lock);
        while (!(rcvr_flags & rcb->rcvr_mask))
        {
            pthread_cond_wait (&iqready_cond, &iqready_lock);
        }
        rcvr_flags &= ~rcb->rcvr_mask;
        pthread_mutex_unlock (&iqready_lock);

        // can happen when switching between rcvr numbers
        if (rcb->iqSamples_remaining < 0)
            rcb->iqSamples_remaining = 0;

        if (!using_IQfiles)
        {
            // downsample starting at any remaining offset
            downsample (rcb);

            switch (mcb.output_rate)
            {
            case 48000:
                rcb->iqSamples_remaining += RTL_READ_COUNT / (DOWNSAMPLE_192 * 8);
                break;

            case 96000:
                rcb->iqSamples_remaining += RTL_READ_COUNT / (DOWNSAMPLE_192 * 4);
                break;

            case 192000:
                rcb->iqSamples_remaining += RTL_READ_COUNT / (DOWNSAMPLE_192 * 2);
                break;

            case 384000:
                rcb->iqSamples_remaining += RTL_READ_COUNT / DOWNSAMPLE_192;
                break;
            }
        }
        else
        {
            memcpy(&rcb->iqSamples[rcb->iqSamples_remaining * 2], &rcb->iq_buf[0], 9600 * 2 * sizeof (float));
            //RRK rcb->iqSamples_remaining += mcb.output_rate * 2;
            rcb->iqSamples_remaining += 9600;
        }

        while (rcb->iqSamples_remaining > samps_packet)
        {
            load_packet (rcb);
            rcb->iqSamples_remaining -= samps_packet;
            rcb->iqSample_offset += samps_packet;
        }

        // move remaining samples to beginning of buffer
        if ((rcb->iqSample_offset > 0) && (rcb->iqSamples_remaining > 0))
        {
            memcpy (&(rcb->iqSamples[0]),
                    &(rcb->iqSamples[rcb->iqSample_offset * 2]),
                    rcb->iqSamples_remaining * 2 * sizeof (float));
            rcb->iqSample_offset = 0;
        }

        // Set new frequency if one is pending and enough time has expired
        if (!using_IQfiles)
        {
            if ((mcb.rcb[rcb->rcvr_num].new_freq) && (mcb.rcb[rcb->rcvr_num].new_freq != 10000000))
            {
                ftime (&mcb.freq_ttime[rcb->rcvr_num]);
                if ((((((mcb.freq_ttime[rcb->rcvr_num].time * 1000) +
                        mcb.freq_ttime[rcb->rcvr_num].millitm) -
                        (mcb.freq_ltime[rcb->rcvr_num].time * 1000) +
                        mcb.freq_ltime[rcb->rcvr_num].millitm)) > 200)
                        || hpsdr_sequence < 1000000)
                {
                    i = mcb.rcb[rcb->rcvr_num].new_freq + mcb.up_xtal +
                        mcb.freq_offset[rcb->rcvr_num];
                    i = rtlsdr_set_center_freq (mcb.rcb[rcb->rcvr_num].rtldev, i);
                    if (i < 0)
                    {
                        t_print ("WARNING: Failed to set rcvr %d to freq %d with offset %+d\n",
                         mcb.rcb[rcb->rcvr_num].rcvr_num + 1,
                         mcb.rcb[rcb->rcvr_num].new_freq,
                         mcb.freq_offset[rcb->rcvr_num]);
                    }

                    mcb.rcb[rcb->rcvr_num].curr_freq = mcb.rcb[rcb->rcvr_num].new_freq;
                    mcb.rcb[rcb->rcvr_num].new_freq = 0;

#if 0
                    rxnum = rcb->rcvr_num;
                    t_print ("INFO: Rx[%d]: if_bw: %dHz, offset: %dHz, center: %dHz [%8dHz], gain: %0.2f dB, gain_mode: %s, agc: %s, bias_t: %s, direct: %s\n",
                     mcb.rcb[rxnum].rcvr_num + 1,
                     mcb.if_bw[rxnum],
                     mcb.freq_offset[rxnum],
                     mcb.rcb[rxnum].curr_freq + mcb.up_xtal + mcb.freq_offset[rxnum],
                     mcb.rcb[rxnum].curr_freq + mcb.freq_offset[rxnum],
                     mcb.gain[rxnum] / 10.0,
                     mcb.gain_mode[rxnum] ? "auto" : "manual",
                     mcb.agc_mode[rxnum] ? "on" : "off",
                     mcb.bias_t[rxnum] ? "on" : "off",
                     mcb.direct_mode[rxnum] ? "on" : "off");
#endif
                }
            }
        }
    }

    pthread_exit (NULL);
    //t_print("EXITING hpsdrsim_sendiq_thr_func() rcvr_mask%d...\n", rcb->rcvr_mask);
}

void
hpsdrsim_stop_threads ()
{
    int i;

    running = 0;

    for (i = 0; i < mcb.total_num_rcvrs; i++)
    {
        rtlsdr_cancel_async (mcb.rcb[i].rtldev);
        pthread_cancel (mcb.rcb[i].rtl_read_thr);
        pthread_cancel (mcb.rcb[i].hpsdrsim_sendiq_thr);
    }

    // unblock held mutexes so we can exit
#if 0
    pthread_mutex_lock (&do_cal_lock);
    mcb.cal_state = CAL_STATE_EXIT;
    pthread_cond_broadcast (&do_cal_cond);
    pthread_mutex_unlock (&do_cal_lock);
    pthread_cancel (do_cal_thr);
#endif

    pthread_mutex_lock (&send_lock);
    send_flags = mcb.rcvrs_mask;
    pthread_cond_broadcast (&send_cond);
    pthread_mutex_unlock (&send_lock);

    pthread_mutex_lock (&iqready_lock);
    rcvr_flags = mcb.rcvrs_mask;
    pthread_cond_broadcast (&iqready_cond);
    pthread_mutex_unlock (&iqready_lock);

    if (mcb.calibrate)
    {
        t_print ("\nfreq_offset ");
        for (i = 0; i < mcb.active_num_rcvrs; i++)
            t_print ("%d%s", mcb.freq_offset[i],
                    (mcb.active_num_rcvrs - 1 != i) ? "," : "\n\n");
    }
}

void update_config()
{
    int i, r;
    for (i = 0; i < mcb.total_num_rcvrs; i++)
    {

        mcb.last_if_bw[i] = mcb.if_bw[i];
        mcb.last_gain[i] = mcb.gain[i];
        mcb.last_gain_mode[i] = mcb.gain_mode[i];
        mcb.last_freq_offset[i] = mcb.freq_offset[i];
        mcb.last_agc_mode[i] = mcb.agc_mode[i];
        mcb.last_bias_t[i] = mcb.bias_t[i];
        mcb.last_direct_mode[i] = mcb.direct_mode[i];
        mcb.last_center_freq[i] = mcb.center_freq[i];
    }
}

// Supported gain values (22): -6.6 -2.3 0.0 4.1 8.2 11.7 15.8 20.9 24.0 29.2 31.9 32.4 37.5 39.9 42.5 43.9 47.1 50.7 54.1 57.6 62.4 66.1
// Supported bandwidth values (9): 300000 400000 550000 700000 1000000 1200000 1300000 1600000 2200000

int
update_dongle ()
{
    int i, r;
    char num[16];
    rtlsdr_dev_t *rtldev;

    t_print("\n");

    for (i = 0; i < mcb.total_num_rcvrs; i++)
    {

        rtldev = mcb.rcb[i].rtldev;

	if (mcb.gain_mode[i] > 0 ) {
	  // Enable Auto Gain Mode
	  r = verbose_auto_gain (rtldev);
	  if (r < 0) {
	    printf ("WARNING: Failed to set tuner gain!\n");
	    return (-1);
	  }
	}
	else {
	  // Set Manual Gain Mode and Gain setting value
	  r = verbose_gain_set (rtldev, mcb.gain[i]);
	  if (r < 0) {
	    printf ("WARNING: Failed to set tuner manual gain!\n");
	    return (-1);
	  }
	}

        // First read current center freq as it may have changed manually
        // independently from change in config file.
        mcb.last_center_freq[i] = rtlsdr_get_center_freq (rtldev);

        // Subtract out the last offset
        mcb.center_freq[i] = mcb.last_center_freq[i] - mcb.last_freq_offset[i];

        // Add in our new freq_offset
        mcb.center_freq[i] += mcb.freq_offset[i];

        // Now write our updated center freq; phew...
        r = rtlsdr_set_center_freq (rtldev, mcb.center_freq[i]);
        if (r < 0)
        {
            t_print ("WARNING: Failed to set tuner freq to %dhz!\n",
                    mcb.center_freq[i]);
            return (-1);
        }

        r = rtlsdr_set_direct_sampling (rtldev, mcb.direct_mode[i]);
        if (r < 0)
        {
            t_print ("WARNING: Failed to set direct sampling!\n");
            return (-1);
        }

        r = rtlsdr_set_agc_mode (rtldev, mcb.agc_mode[i]);
        if (r < 0)
        {
            t_print ("WARNING: Failed to set automatic gain!\n");
            return (-1);
        }

        r = rtlsdr_set_bias_tee (rtldev, mcb.bias_t[i]);
        if (r < 0)
        {
            t_print ("WARNING: Failed to set bias T!\n");
            return (-1);
        }

        sprintf (num, "%d", mcb.gain[i]);

#if 0
        //      gain = mcb.gain[i];

        t_print ("INFO: Rx[%d]: if_bw: %dHz, center: %dHz [%8dHz], gain: %0.2f dB, gain_mode: %s, agc: %s, bias_t: %s, direct: %s\n",
         mcb.rcb[i].rcvr_num + 1,
         mcb.if_bw[i],
         mcb.center_freq[i],
         mcb.center_freq[i] - mcb.up_xtal,
         mcb.gain[i] / 10.0,
         mcb.gain_mode[i] ? "auto" : "manual",
         mcb.agc_mode[i] ? "on" : "off",
         mcb.bias_t[i] ? "on" : "off",
         mcb.direct_mode[i] ? "on" : "off");
#endif

    }

    return 0;
}

void *
do_cal_thr_func (void *arg)
{
    struct rcvr_cb *rcb;
    int i, n, tsleep;
    float var = 3000.0f; // freq in hz from the calibration freq we care about
    float magnitude, last, current;
    float bin = (float) RTL_SAMPLE_RATE / FFT_SIZE;
    static int min_offset = 0, max_offset = 3000;
    static u_int first_pass = 1, first_pass_mask = 0;
    static int flip_offset[2][MAX_RCVRS];
    static int flip[MAX_RCVRS] = { 0 }, no_signal = 0;

    //t_print("ENTERING do_cal_thr_func()\n");

    while (!do_exit && mcb.calibrate)
    {
        // we may do this if we re-parse the config file
        if (reset_cal)
        {
            reset_cal = 0;
            do_cal_time = 5;       // TODO, this needs a mutex
            first_pass = 1;
            first_pass_mask = 0;
            no_signal = 0;
            min_offset = 0;
            max_offset = 3000;
            memset (flip_offset, 0, sizeof (flip_offset));
            memset (flip, 0, sizeof (flip));
        }

        pthread_mutex_lock (&do_cal_lock);
        while (mcb.cal_state < 0)
        {
            pthread_cond_wait (&do_cal_cond, &do_cal_lock);
        }
        pthread_mutex_unlock (&do_cal_lock);
        if (mcb.cal_state == CAL_STATE_EXIT)
            goto EXIT_DO_CAL;

        rcb = &mcb.rcb[mcb.cal_state];

        //t_print("do_cal_thr_func() STATE 1 rcvr_num: %d\n", rcb->rcvr_num);

        // this is an attempt to check whether the user wants to calibrate
        // from the upconvertor xtal frequency or an HF station
#if 1
        if ((mcb.up_xtal) && (mcb.calibrate < 30000000))
            i = mcb.up_xtal + mcb.calibrate;
        else
            i = mcb.calibrate;
        tsleep = 10000 + (abs (i - (rcb->curr_freq + mcb.up_xtal)) / 1000);

        // give some time for the dongle to lock in
        rtlsdr_set_center_freq (rcb->rtldev, i);
        usleep (tsleep);
#else
        i = (abs (mcb.up_xtal - mcb.calibrate) > var)
            ? mcb.up_xtal + mcb.calibrate : mcb.calibrate;
        rtlsdr_set_center_freq (rcb->rtldev, i);
#endif

        pthread_mutex_lock (&do_cal_lock);
        mcb.cal_state = CAL_STATE_1;
        while (mcb.cal_state == CAL_STATE_1)
        {
            pthread_cond_wait (&do_cal_cond, &do_cal_lock);
        }
        pthread_mutex_unlock (&do_cal_lock);
        if (mcb.cal_state == CAL_STATE_EXIT)
            goto EXIT_DO_CAL;

        // reset to the last calibrated freq to reduce the gap between
        // capturing actual data, after the dongle frequencies have settled
        rtlsdr_set_center_freq (rcb->rtldev,
                                rcb->curr_freq + mcb.up_xtal +
                                mcb.freq_offset[rcb->rcvr_num]);

        //t_print("do_cal_thr_func() STATE 2 rcvr_num: %d\n", rcb->rcvr_num);

        for (n = 0; n < (RTL_READ_COUNT / 2) / FFT_SIZE; ++n)
        {
            for (i = 0; i < FFT_SIZE; ++i)
            {
                // Normalize the IQ data and put it as the complex FFT input
                mcb.fftIn[i][0] =
                    ((float) fft_buf[i * 2 + n * FFT_SIZE] - 127.0f) * 0.008f;
                mcb.fftIn[i][1] =
                    ((float) fft_buf[i * 2 + n * FFT_SIZE + 1] - 127.0f) * 0.008f;
            }
            fftw_execute (mcb.fftPlan);
            for (i = 0; i < FFT_SIZE; ++i)
            {
                // Calculate the logarithmic magnitude of the complex FFT output
                magnitude = 0.05 * log (mcb.fftOut[i][0] * mcb.fftOut[i][0]
                                        + mcb.fftOut[i][1] * mcb.fftOut[i][1] +
                                        1.0);

                // Average the signal
                //averaged[i] -= 0.01f * (averaged[i] - magnitude);
                mcb.fft_averaged[i] = magnitude;
            }
        }

        last = 0.0f;
        for (i = (FFT_SIZE / 2) - (var / bin); i < (FFT_SIZE / 2) + (var / bin);
                ++i)
        {
            current = mcb.fft_averaged[(i + FFT_SIZE / 2) % FFT_SIZE];
            if (current > last)
            {
                //t_print("DEBUG c:%f l:%f n:%d i:%d\n", current, last, n, i);
                last = current;
                n = i;
            }
        }
        current = (float) mcb.calibrate + ((n - (FFT_SIZE / 2)) * bin);
        i = (int) current - mcb.calibrate;
        //t_print("rcvr:%d i:%d n:%d l:%f c:%f o:%d\n", rcb->rcvr_num+1, i, n, last, current, (int)(current - mcb.calibrate));
        cal_rcvr_mask ^= 1 << rcb->rcvr_num;

        no_signal = (((FFT_SIZE / 2) == n)
                     && (current == mcb.calibrate)) ? no_signal + 1 : 0;
        if (10 == no_signal)
        {
            no_signal = 0;
            t_print ("WARNING:  *** Calibration signal on %d Hz is very weak,"
                    " consider changing it! ***\n", mcb.calibrate);
        }

        // if n == 0 we won't update mcb.freq_offset[rcb->rcvr_num]
        n = abs (mcb.freq_offset[rcb->rcvr_num] - i);

        // only update offset if it's been established and we're not changing it drastically
        // also avoid flipping back and forth to a previous state
        if (i && (n < max_offset) && (n > min_offset)
                && (i != flip_offset[1][rcb->rcvr_num])
                && (i != flip_offset[0][rcb->rcvr_num]))
        {
            rtlsdr_set_center_freq (rcb->rtldev,
                                    rcb->curr_freq + mcb.up_xtal + i);
            //t_print
            //  ("INFO: [%s] cal update, rcvr %d old offset %+5d new offset %+5d new freq %d\n",
            //  time_stamp (), rcb->rcvr_num + 1, mcb.freq_offset[rcb->rcvr_num],
            //  i, rcb->curr_freq + i);

            if (2 == flip[rcb->rcvr_num])
            {
                flip_offset[0][rcb->rcvr_num] = flip_offset[1][rcb->rcvr_num];
                flip_offset[1][rcb->rcvr_num] = i;
            }
            else
            {
                flip_offset[flip[rcb->rcvr_num]][rcb->rcvr_num] = i;
                flip[rcb->rcvr_num] += 1;
            }

            mcb.freq_offset[rcb->rcvr_num] = i;

            // after first pass cut back on the allowable offset
            if (first_pass)
            {
                first_pass_mask |= 1 << rcb->rcvr_num;
                if (first_pass_mask == mcb.rcvrs_mask)
                {
                    first_pass = 0;
                    min_offset = ((int) bin) - 1;
                    max_offset = 200;
                }
            }

        }
        else
        {
#if 0
            t_print ("INFO: [%s] NO cal update, rcvr %d old offset %+5d new offset %+5d new freq %d flip0 %d\n",
             time_stamp (), rcb->rcvr_num + 1, mcb.freq_offset[rcb->rcvr_num],
             i, rcb->curr_freq + i, flip_offset[0][rcb->rcvr_num]);
#endif
        }

        mcb.cal_state = CAL_STATE_3;
        //t_print("do_cal_thr_func() STATE 3 rcvr_num: %d\n", rcb->rcvr_num);
    }
EXIT_DO_CAL:
    pthread_exit (NULL);
    //t_print("EXITING do_cal_thr_func()\n");
}

void
rtlsdr_callback (unsigned char *buf, uint32_t len, void *ctx)
{
    int i;
    struct rcvr_cb *rcb = (struct rcvr_cb *) ctx;
    float dcI, dcQ;

    if (do_exit || !running)
    {
        return;
    }

    if (RTL_READ_COUNT != len)
    {
        perror ("rtlsdr_callback(): RTL_READ_COUNT != len!\n");
        return;
    }

#if 0
    ftime (&test_end_time);
    t_print ("test time %ld ms\n",
            ((test_end_time.time * 1000) + test_end_time.millitm) -
            ((test_start_time.time * 1000) + test_start_time.millitm));
    ftime (&test_start_time);
#endif

    // periodically calibrate each dongle if enabled
    if (mcb.calibrate)
    {
        cal_count[rcb->rcvr_num] += 1;

        if ((cal_rcvr < 0) || (cal_rcvr == rcb->rcvr_num))
        {

            if (mcb.cal_state == CAL_STATE_0)
            {
                if ((cal_rcvr_mask & (1 << rcb->rcvr_num)) &&
                        (cal_count[rcb->rcvr_num] > (do_cal_time * 20)))
                {
                    cal_rcvr = rcb->rcvr_num;
                    pthread_mutex_lock (&do_cal_lock);
                    mcb.cal_state = rcb->rcvr_num;
                    pthread_cond_broadcast (&do_cal_cond);
                    pthread_mutex_unlock (&do_cal_lock);
                    //t_print("rtlsdr_callback() STATE 1 cmask %x\n", cal_rcvr_mask);
                }
            }
            else if (mcb.cal_state == CAL_STATE_1)
            {
                pthread_mutex_lock (&iqready_lock);
                rcvr_flags |= rcb->rcvr_mask;
                pthread_cond_broadcast (&iqready_cond);
                pthread_mutex_unlock (&iqready_lock);
                //t_print("rtlsdr_callback() STATE 2 cmask %x\n", cal_rcvr_mask);
//RRK just pass buf to save time? seems to work OK
                //memcpy(fft_buf, buf, RTL_READ_COUNT);
                fft_buf = buf;
                pthread_mutex_lock (&do_cal_lock);
                mcb.cal_state = CAL_STATE_2;
                pthread_cond_broadcast (&do_cal_cond);
                pthread_mutex_unlock (&do_cal_lock);
                return;
            }
            else if (mcb.cal_state == CAL_STATE_3)
            {
                //t_print("rtlsdr_callback() STATE 3 cmask %x\n", cal_rcvr_mask);
                mcb.cal_state = CAL_STATE_0;
                cal_rcvr = -1;
                // do_cal_thr_func() will clear this bit on successful cal
                if (!(cal_rcvr_mask & (1 << rcb->rcvr_num)))
                    cal_count[rcb->rcvr_num] = 0;
            }

            if (cal_rcvr_mask == 0)
            {
                cal_rcvr_mask = mcb.rcvrs_mask;
                // gradually increase cal time to a max of 30 minutes
                do_cal_time = (do_cal_time >= 1800) ? 1800 : do_cal_time + 1;
            }
        }
    }

    // Convert to float and copy data to buffer, offset by coefficient length * 2.
    // The downsample routine will move the previous last coefficient length * 2
    // to the beginning of the buffer. This is because of the FIR filter length, the
    // filtering routine takes in 'filter_length' more samples than it outputs or
    // coefficient length * 2 for I&Q (stereo) input samples.
    for (i = 0; i < RTL_READ_COUNT; i += 2)
    {
#if 1                           // remove DC component
        dcI = rtl_lut[last_rate][buf[i + 1]] + dcIlast[rcb->rcvr_num] * 0.9999f;
        dcQ = rtl_lut[last_rate][buf[i]] + dcQlast[rcb->rcvr_num] * 0.9999f;
        rcb->iq_buf[i] = dcI - dcIlast[rcb->rcvr_num];
        rcb->iq_buf[i + 1] = dcQ - dcQlast[rcb->rcvr_num];
        dcIlast[rcb->rcvr_num] = dcI;
        dcQlast[rcb->rcvr_num] = dcQ;
#else
        rcb->iq_buf[i + 1] = rtl_lut[last_rate][buf[i]];
        rcb->iq_buf[i] = rtl_lut[last_rate][buf[i + 1]];
#endif
    }

    // below pthread_mutex_lock may have to be before loop above? TODO
    pthread_mutex_lock (&iqready_lock);
    rcvr_flags |= rcb->rcvr_mask;
    pthread_cond_broadcast (&iqready_cond);
    pthread_mutex_unlock (&iqready_lock);
}

void
format_payload (void)
{
    int i;
    u_char hpsdr_header[8] = { 0xEF, 0xFE, 1, 6, 0, 0, 0, 0 };
    u_char proto_header[8] =
    { 0x7f, 0x7f, 0x7f, 0, 0x1e, 0, 0, HERMES_FW_VER };

    for (i = 0; i < HPSDR_FRAME_LEN; i++)
        payload[i] = 0;

    for (i = 0; i < 8; i++)
        payload[i] = hpsdr_header[i];

    for (i = 8; i < 15; i++)
        payload[i] = proto_header[i - 8];

    for (i = 520; i < 527; i++)
        payload[i] = proto_header[i - 520];
}

void *
rtl_read_thr_func (void *arg)
{
    struct rcvr_cb *rcb = (struct rcvr_cb *) arg;
    int r, j, k, i = rcb->rcvr_num;
    FILE * fp;
    unsigned short IQm, IQl;
    short IQ;
    int ns_interval;
    struct timespec timeS, timeE;
    time_t raw_time;
    struct tm *ptr_ts;
    WavHeader header;
    bool wav = false, do_zero = false;;
    char message[MAXSTR] = {0,};


    //t_print("ENTERING rtl_read_thr_func() rcvr %d\n", i+1);
#if 1
    if (using_IQfiles)
    {
        if ((fp = fopen(rcb->filename, "rb")) != NULL)
        {
            while (!do_exit)
            {
                j = k = 0;
                if (strstr(rcb->filename, ".wav") != NULL)
                {
                    fread(&header, sizeof(WavHeader), 1, fp);
                    wav = true;
                }
                clock_gettime(CLOCK_MONOTONIC, &timeS);
                while (running)
                {
                    if (do_zero) rcb->iq_buf[j] = rcb->iq_buf[j+1] = ((float)rand()/(float)(RAND_MAX)) * 65536.0f;
                    else if (wav)   //RRK TODO, set sample rate/size from header info
                    {
                        fread(&IQ, sizeof(IQ), 1, fp);
                        rcb->iq_buf[j] = (float)((IQ << 16) & 0xffff0000);
                        fread(&IQ, sizeof(IQ), 1, fp);
                        r = (int)IQ;
                        rcb->iq_buf[j+1] = (float)((IQ << 16) & 0xffff0000);
                    }
                    else
                    {
                        fread(&IQm, sizeof(IQm), 1, fp);
                        fread(&IQl, sizeof(IQm), 1, fp);
                        rcb->iq_buf[j] = (float)((IQm << 16) | IQl);
                        fread(&IQm, sizeof(IQm), 1, fp);
                        fread(&IQl, sizeof(IQm), 1, fp);
                        rcb->iq_buf[j+1] = (float)((IQm << 16) | IQl);
                    }
                    j+=2;
                    k++;
                    // set for N8UR's bin recordings @ 96ksps
                    if (k == 9600)
                    {
                        pthread_mutex_lock (&iqready_lock);
                        rcvr_flags |= rcb->rcvr_mask;
                        pthread_cond_broadcast (&iqready_cond);
                        pthread_mutex_unlock (&iqready_lock);
                        break;
                    }
                    if ((message[0] == 0) && feof(fp))
                    {
                        if (using_IQrewind)
                        {
                            rewind(fp);
                            strcpy(message, "Rewinding");
                        }
                        else
                        {
                            strcpy(message, "Ending");
                        }
                        time (&raw_time);
                        ptr_ts = gmtime (&raw_time);

                        // format time to SkimSrv's i.e. 2018-02-01 16:52:05Z
                        t_print("rcvr %d, %s file %s at %4d-%02d-%02d %2d:%02d:%02dZ\n",
                               i+1, message, rcb->filename, ptr_ts->tm_year+1900, ptr_ts->tm_mon,
                               ptr_ts->tm_mday, ptr_ts->tm_hour, ptr_ts->tm_min, ptr_ts->tm_sec);

                        if (!strcmp("Ending", message))
                        {
                            fclose(fp);
                            do_zero = true; // just fill with noise
                        }
                    }
                }
                clock_gettime(CLOCK_MONOTONIC, &timeE);
                ns_interval = (timeE.tv_sec > timeS.tv_sec) ? ((timeE.tv_sec - timeS.tv_sec) * 1000000000)
                              - (timeS.tv_nsec - timeE.tv_nsec)
                              : (timeE.tv_nsec - timeS.tv_nsec);
                // figure out the delta from above, we want to sleep precisely 100ms
                timeE.tv_nsec += 100000000 - ns_interval;
                if(timeE.tv_nsec >= 1000000000)
                {
                    timeE.tv_nsec -= 1000000000;
                    timeE.tv_sec++;
                }
                clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &timeE, NULL);
            }
            fclose(fp);
        }
    }
    else
    {
        r = rtlsdr_read_async (rcb->rtldev, rtlsdr_callback,
                               (void *) (&mcb.rcb[i]), 2, RTL_READ_COUNT);
    }
#else // simulate an rtl read
    while (!do_exit || running)
    {
        usleep (5000);            // approximate
        pthread_mutex_lock (&iqready_lock);
        rcvr_flags |= rcb->rcvr_mask;
        pthread_cond_broadcast (&iqready_cond);
        pthread_mutex_unlock (&iqready_lock);
    }
    static u_char buf[RTL_READ_COUNT];

    while (!do_exit || running)
    {
        r = rtlsdr_read_sync (rcb->rtldev, buf, RTL_READ_COUNT, &i);
        if (RTL_READ_COUNT != i)
        {
            perror ("rtlsdr_callback(): RTL_READ_COUNT != i!\n");
        }

        pthread_mutex_lock (&iqready_lock);

        for (i = 0; i < RTL_READ_COUNT; i += 2)
        {
            rcb->iq_buf[i + 1] = rtl_lut[buf[i]];
            rcb->iq_buf[i] = rtl_lut[buf[i + 1]];
        }
        rcvr_flags |= rcb->rcvr_mask;
        pthread_cond_broadcast (&iqready_cond);
        pthread_mutex_unlock (&iqready_lock);
    }
#endif
    //t_print("EXITING rtl_read_thr_func() rcvr %d\n", i+1);
    pthread_exit (NULL);
}

int
init_rtl (int rcvr_num, int dev_index)
{
    int r;
    char num[16];
    rtlsdr_dev_t *rtldev;

    r = rtlsdr_open (&(mcb.rcb[rcvr_num].rtldev), dev_index);

    if (r < 0)
    {
        t_print ("ERROR:  Failed to open rtlsdr device\n");
        return (-1);
    }

    rtldev = mcb.rcb[rcvr_num].rtldev;

    if (mcb.if_bw[rcvr_num] == 0)
    {
        t_print ("Setting tuner IF BW to %d Hz\n", RTL_BANDWIDTH);
        r = rtlsdr_set_tuner_bandwidth (rtldev, RTL_BANDWIDTH);
        if (r < 0)
        {
            t_print ("WARNING: Failed to set IF BW to %d Hz!\n",RTL_BANDWIDTH);
            return (-1);
        }

        mcb.if_bw[rcvr_num] = RTL_BANDWIDTH;
    }
    else
    {
        t_print ("Setting tuner IF BW to %d Hz\n", mcb.if_bw[rcvr_num]);
        r = rtlsdr_set_tuner_bandwidth (rtldev, mcb.if_bw[rcvr_num]);
        if (r < 0)
        {
            t_print ("WARNING: Failed to set IF BW to %d Hz!\n",mcb.if_bw[rcvr_num]);
            return (-1);
        }
    }


    r = rtlsdr_set_sample_rate (rtldev, RTL_SAMPLE_RATE);

    if (r < 0)
    {
        t_print ("WARNING: Failed to set sample rate to %d!\n", RTL_SAMPLE_RATE);
        return (-1);
    }

    sprintf (num, "%d", mcb.gain[rcvr_num]);

    if (mcb.gain_mode[rcvr_num] > 0) {
      // Enable Auto Gain Mode
      r = verbose_auto_gain (rtldev);
      if (r < 0) {
          printf ("WARNING: Failed to set tuner gain to auto!\n");
          return (-1);
      } else printf ("  tuner gain\t\tauto\n");
    } else {
        // Set Manual Gain Mode and Gain setting value
        r = verbose_gain_set (rtldev, mcb.gain[rcvr_num]);
        if (r < 0) {
            printf ("WARNING: Failed to set tuner manual gain!\n");
            return (-1);
        } else printf ("  tuner gain\t\t%0.2f dB\n", mcb.gain[rcvr_num]/10.0);
    }

    rtlsdr_set_center_freq (rtldev, 100000000);
    if (r < 0)
        t_print ("WARNING: Failed to set tuner freq to 100000000hz!\n");

    r = rtlsdr_set_direct_sampling (rtldev, mcb.direct_mode[rcvr_num]);

    if (r < 0)
    {
        t_print ("WARNING: Failed to set direct sampling!\n");
        return (-1);
    }
    else
        t_print ("  direct sampling\t%d\n", mcb.direct_mode[rcvr_num]);

    r = rtlsdr_set_agc_mode (rtldev, mcb.agc_mode[rcvr_num]);

    if (r < 0)
    {
        t_print ("WARNING: Failed to set automatic gain!\n");
        return (-1);
    }
    else
        t_print ("  agc mode\t\t%s\n",
                (mcb.agc_mode[rcvr_num]) ? "on" : "off");

    r = rtlsdr_set_bias_tee (rtldev, mcb.bias_t[rcvr_num]);

    if (r < 0)
    {
        t_print ("WARNING: Failed to set bias T!\n");
        return (-1);
    }
    else
        t_print ("  bias_t\t\t%s\n\n",
                (mcb.bias_t[rcvr_num]) ? "on" : "off");

    r = rtlsdr_reset_buffer (rtldev);

    if (r < 0)
    {
        t_print ("WARNING: Failed to reset buffers!\n");
        return (-1);
    }

    return (0);
}

void
usage (char *progname)
{
    t_print
    ("\n%s, an HPSDR Hermes simulator for RTL2832 based DVB-T receivers",
     progname);
    t_print ("\nSee rtl_hpsdr.conf for configuration option descriptions.\n"
            "\nUsage:\n" "\tPer rcvr options (comma separated i.e. 1,0,1,1):\n"
            "\t[-a internal agc of the rtl2832 0|1 (defaults 0 or off)]\n"
            "\t[-b turn on bias_t (if supported by dongle) 0|1 (defaults 0 or off)]\n"
            "\t[-d direct sampling mode 0|1|2|3 (defaults 0 or auto, 1=I 2=Q 3=NOMOD)]\n"
            "\t[-f freq offset in hz (defaults 0)]\n"
            "\t[-g gain in tenths of a db (defaults 0 for auto)]\n"
            "\t[-o rcvr order (defaults to 1 - number detected)]\n"
            "\t[-w IQ_filename (file plays to end then stops)]\n"
            "\t[-W IQ_filename (file plays endlessly in a loop)]\n\n"
            "\tGlobal options:\n"
            "\t[-C freq in hz in which to calibrate selected dongles]\n"
            "\t[-c path to config file (overrides these options)]\n"
            "\t[-e use serial eeprom number]\n"
            "\t[-h help (prints this usage)]\n"
            "\t[-l freq in hz of the upconvertor xtal]\n"
            "\t[-m signal multiplier (default 1)]\n"
            "\t[-r number of rcvrs to use (defaults to all detected)]\n"
#ifndef _WIN32
            "\t[-s sound device (alsa) for audio (i.e. plughw:0,0 defaults to none)]\n"
#endif
            "\t[-v print out version info]\n\n");
    exit (1);
}

int
set_option (int *option, char *value)
{
    char params[MAX_RCVRS][MAXSTR];
    int i, count = 0;
    char *token;
    const char s[2] = ",";

    // get the first token
    token = strtok (value, s);

    // walk through other tokens
    while ((token != NULL) && (count < MAX_RCVRS))
    {
        strcpy (&(params[count++][0]), token);
        token = strtok (NULL, s);
    }

    for (i = 0; i < MAX_RCVRS; i++)
    {
        if (i < count)
        {
            option[i] = atoi (params[i]);
            option[MAX_RCVRS] = option[i]; // save last
        }
        else
            option[i] = option[MAX_RCVRS]; // set to last
    }

    return (count);
}

int
set_gain_option (int *option, char *value)
{
    char params[MAX_RCVRS][MAXSTR];
    int i, count = 0;
    char *token;
    const char s[2] = ",";

    // get the first token
    token = strtok (value, s);

    // walk through other tokens
    while ((token != NULL) && (count < MAX_RCVRS))
    {
        strcpy (&(params[count++][0]), token);
        token = strtok (NULL, s);
    }

    for (i = 0; i < MAX_RCVRS; i++)
    {
        if (i < count)
        {
            option[i] = (int)(atof(params[i]) * 10); /* tenths of a dB */
            option[MAX_RCVRS] = option[i]; // save last
        }
        else
            option[i] = option[MAX_RCVRS]; // set to last
    }

    return (count);
}

int
set_filename_option (char *value)
{
    char params[MAX_RCVRS][MAXSTR];
    int i, count = 0;
    char *token;
    const char s[2] = ",";

    // get the first token
    token = strtok (value, s);

    // walk through other tokens
    while ((token != NULL) && (count < MAX_RCVRS))
    {
        strcpy (&(params[count++][0]), token);
        token = strtok (NULL, s);
    }

    for (i = 0; i < MAX_RCVRS; i++)
    {
        if (i < count)
        {
            strcpy(mcb.rcb[i].filename, params[i]);
            if(access(mcb.rcb[i].filename, F_OK) == -1 )
            {
                t_print("ERROR File: %s does not exist!\n", mcb.rcb[i].filename);
                exit(-1);
            }
            mcb.total_num_rcvrs += 1;
        }
    }

    return (count);
}

int
parse_config (char *conf_file)
{
    FILE *fp;
    int count, line = 0;
    char option[MAXSTR], value[MAXSTR];
    char confbuf[MAXSTR];

    if ((fp = fopen (conf_file, "r")) != NULL)
    {
        //      t_print ("\nParsing config file %s\n\n", conf_file);

        while (fgets (confbuf, MAXSTR, fp) != NULL)
        {
            line++;

            if (strchr (confbuf, '#') != NULL)
                continue;

            option[0] = value[0] = '\0';
            sscanf (confbuf, "%s %s", option, value);

            if (!strcmp ("direct_mode", option))
            {
                count = set_option (mcb.direct_mode, value);
            }
            else if (!strcmp ("tuner_gain", option))
            {
                count = set_gain_option (mcb.gain, value);
            }
            else if (!strcmp ("freq_offset", option))
            {
                count = set_option (mcb.freq_offset, value);
            }
            else if (!strcmp ("agc_mode", option))
            {
                count = set_option (mcb.agc_mode, value);
            }
            else if (!strcmp ("bias_t", option))
            {
                count = set_option (mcb.bias_t, value);
            }
            else if (!strcmp ("rcvr_order", option))
            {
                count = set_option (mcb.rcvr_order, value);
            }
            else if (!strcmp ("sound_dev", option))
            {
                strcpy (mcb.sound_dev, value);
            }
            else if (!strcmp ("signal_multiplier", option))
            {
                mcb.signal_multiplier = atoi (value);
            }
            else if (!strcmp ("total_num_rcvrs", option))
            {
                mcb.total_num_rcvrs = atoi (value);
            }
            else if (!strcmp ("up_xtal", option))
            {
                mcb.up_xtal = atoi (value);
            }
            else if (!strcmp ("calibrate", option))
            {
                mcb.calibrate = atoi (value);
            }
            else if (!strcmp ("ip_addr", option))
            {
                strcpy (mcb.ip_addr, value);
            }
            else if (!strcmp ("rtl_mode", option))
            {
                mcb.rtl_mode = atoi (value);
            }
            else if (!strcmp ("if_bw", option))
            {
                count = set_option (mcb.if_bw, value);
            }
            else if (!strcmp ("gain_mode", option))
            {
                count = set_option (mcb.gain_mode, value);
            }
        }

        return (0);
    }
    else
    {
        t_print ("Cannot find %s\n", conf_file);
        exit (-1);
    }
}


int new_protocol_running()
{
    return 0;
}

int main (int argc, char *argv[])
{
    int i, j, r, size;
    int opt, count = 0;
    pthread_t thread;
    uint8_t id[4] = { 0xef, 0xfe, 1, 6 };
    int16_t sample;
    struct sockaddr_in addr_udp;
    struct sockaddr_in addr_from;
    socklen_t lenaddr;
    struct timeval tv;
    int num_rcvrs, yes = 1;
    uint8_t *bp;
    unsigned long checksum = 0;
    uint32_t last_seqnum = 0xffffffff, seqnum;  // sequence number of received packet
    int udp_retries = 0;
    int bytes_read, bytes_left;
    uint32_t code;
    uint32_t *code0 = (uint32_t *) buffer;  // fast access to code of first buffer
    double run, off, off2, inc;
    struct timeval tvzero = {0, 0};
    const int MAC1 = 0x00;
    const int MAC2 = 0x1C;
    const int MAC3 = 0xC0;
    const int MAC4 = 0xA2;
    int MAC5 = 0x10;
    const int MAC6 = 0xDD;  // P1
    const int MAC6N = 0xDD; // P2
    int OLDDEVICE = ODEV_HERMES;
    int NEWDEVICE = NDEV_HERMES;

    bool loop = true;
    char serialstr[MAXSTR];
    char *progname = basename (argv[0]);
    char vendor[256] = { 0 }, product[256] = { 0 }, serial[256] = { 0 };
    struct sigaction sigact;

    // set defaults
    mcb.sound_dev[0] = 0;
    conf_file[0] = 0;
    mcb.output_rate = 48000;
    mcb.serialstr[0] = 0;
    mcb.signal_multiplier = 1;
    mcb.cal_state = CAL_STATE_0;
    mcb.calibrate = 0;
    mcb.up_xtal = 0;
    mcb.rtl_mode = RTL_MODE_SKIMMER;

    ftime (&test_start_time);

    // Initialize per receiver config settings
    for (i = 0; i < MAX_RCVRS; i++)
    {
        mcb.agc_mode[i] = 0;
        mcb.rcb[i].filename[0] = '\0';
        mcb.last_agc_mode[i] = 0;
        mcb.bias_t[i] = 0;
        mcb.last_bias_t[i] = 0;
        mcb.direct_mode[i] = 0;
        mcb.last_direct_mode[i] = 0;
        mcb.gain[i] = 0;
        mcb.last_gain[i] = 0;
        mcb.freq_offset[i] = 0;
        mcb.last_freq_offset[i] = 0;

        mcb.rcvr_order[i] = i + 1;
        copy_rcvr[i] = -1;
        memset (&mcb.freq_ltime[i], 0, sizeof (mcb.freq_ltime[i]));
    }

    while (loop
            && ((opt = getopt (argc, argv, "C:c:a:b:d:e:f:g:h:l:m:o:p:r:s:v:W:w:")) !=
                -1))
    {
        switch (opt)
        {
        case 'a':
            r = set_option (mcb.agc_mode, optarg);
            break;

        case 'W':
            using_IQrewind = true;
            t_print("IQ files will be looped\n");

        case 'w':
            r = set_filename_option (optarg);
            using_IQfiles = true;
            break;

        case 'd':
            r = set_option (mcb.direct_mode, optarg);
            break;

        case 'C':
            mcb.calibrate = atoi (optarg);
            break;

        case 'c':
            strcpy (conf_file, optarg);
            parse_config (conf_file);
            loop = false;
            break;

        case 'e':
            strcpy (mcb.serialstr, optarg);
            break;

        case 'f':
            r = set_option (mcb.freq_offset, optarg);
            break;

        case 'g':
            r = set_option (mcb.gain, optarg);
            break;

        case 'i':
            strcpy (mcb.ip_addr, optarg);
            break;

        case 'l':
            mcb.up_xtal = atoi (optarg);
            break;

        case 'm':
            mcb.signal_multiplier = atoi (optarg);
            break;

        case 'o':
            r = set_option (mcb.rcvr_order, optarg);
            break;

        case 'r':
            mcb.total_num_rcvrs = atoi (optarg);
            break;

        case 's':
            strcpy (mcb.sound_dev, optarg);
            break;

        case 'v':
            t_print ("\nGNU %s Version %s Date Built %s %s\n", progname,
                    PRG_VERSION, __TIME__, __DATE__);
            //t_print ("GIT Hash %s\n", GITVERSION);
            t_print ("Copyright (C) 2014 Free Software Foundation, Inc.\n"
                    "License GPLv3+: GNU GPL version 3 or later <http://gnu.org/licenses/gpl.html>\n"
                    "This is free software: you are free to change and redistribute it.\n"
                    "There is NO WARRANTY, to the extent permitted by law.\n\n");
            exit (0);

        case 'h':
        default:
            usage (progname);
            break;
        }
    }

    if (!using_IQfiles)
    {
        r = rtlsdr_get_device_count ();

        if (r)
        {
            t_print ("Found %d RTL device(s)", r);

            if ((mcb.total_num_rcvrs > r) || !mcb.total_num_rcvrs)
                mcb.total_num_rcvrs = r;

            if (mcb.total_num_rcvrs > MAX_RCVRS)
                mcb.total_num_rcvrs = MAX_RCVRS;

            t_print (", using %d.\n", mcb.total_num_rcvrs);
        }
        else
        {
            t_print ("No RTL devices found, exiting.\n");
            exit (0);
        }

        t_print ("RTL base sample rate: %d hz\n\n", RTL_SAMPLE_RATE);
        t_print ("Global settings:\n");
        t_print ("  config file:\t\t%s\n",
                (0 == conf_file[0]) ? "none" : conf_file);
        t_print ("  ip address:\t\t%s\n", mcb.ip_addr);
        if (0 != mcb.serialstr[0])
            t_print ("  eeprom serial string:\t%s\n", mcb.serialstr);
        t_print ("  number of rcvrs:\t%d\n", mcb.total_num_rcvrs);
        t_print ("  hpsdr output rate:\t%d hz\n", mcb.output_rate);
        t_print ("  signal multiplier\t%d\n", mcb.signal_multiplier);
        if (mcb.calibrate)
            t_print ("  calibration freq:\t%d hz\n", mcb.calibrate);
        if (mcb.up_xtal)
            t_print ("  up_xtal freq:\t\t%d hz\n", mcb.up_xtal);
        t_print ("  sound device:\t\t%s\n",
                (0 == mcb.sound_dev[0]) ? "none" : mcb.sound_dev);
    }
    else
    {
        t_print("Using IQ files, no RTL hardware will be used\n");
    }

    sigact.sa_handler = rtl_sighandler;
    sigemptyset (&sigact.sa_mask);
    sigact.sa_flags = 0;
    sigaction (SIGINT, &sigact, NULL);
    sigaction (SIGTERM, &sigact, NULL);
    sigaction (SIGQUIT, &sigact, NULL);
    sigaction (SIGPIPE, &sigact, NULL);

    format_payload ();

    // create a lookup table for float values
    for (r = 0; r < 4; r++)
    {
        for (i = 0; i < 256; i++)
        {
            rtl_lut[r][i] = (float) (i - 127) *
                            (float) (mcb.signal_multiplier - (r * 10));
        }
    }

    pthread_mutex_init (&iqready_lock, NULL);
    pthread_cond_init (&iqready_cond, NULL);
    pthread_mutex_init (&send_lock, NULL);
    pthread_cond_init (&send_cond, NULL);
    pthread_mutex_init (&done_send_lock, NULL);
    pthread_cond_init (&done_send_cond, NULL);
    pthread_mutex_init (&do_cal_lock, NULL);
    pthread_cond_init (&do_cal_cond, NULL);

    if (mcb.sound_dev[0])
        open_local_sound (mcb.sound_dev);

    // enable the 1st rcvr until we get the active count
    mcb.rcb[0].rcvr_mask = 1;
    mcb.active_num_rcvrs = 1;
    mcb.rcvrs_mask = 1;
    mcb.nsamps_packet = nsamps_packet[0];
    mcb.frame_offset1 = frame_offset1[0];
    mcb.frame_offset2 = frame_offset2[0];

    mcb.fftPlan = fftw_plan_dft_1d (FFT_SIZE, mcb.fftIn,
                                    mcb.fftOut, FFTW_FORWARD, FFTW_ESTIMATE);

    for (i = 0; i < mcb.total_num_rcvrs; i++)
    {
        mcb.rcb[i].mcb = &mcb;
        mcb.rcb[i].new_freq = 0;
        mcb.rcb[i].output_rate = 0;
        t_print ("\nRcvr %d (ordered as %d) settings...\n", i + 1,
                mcb.rcvr_order[i]);
        if (mcb.rcb[i].filename[0] == '\0')
        {
            rtlsdr_get_device_usb_strings (i, vendor, product, serial);
            t_print ("  S/N \t\t\t%s\n", serial);
            t_print ("  freq offset\t\t%d hz\n", mcb.freq_offset[i]);

            if (0 != init_rtl (i, mcb.rcvr_order[i] - 1))
            {
                t_print ("ERROR: Failed init_rtl rcvr%d hardware!\n", i + 1);
                return (-1);
            }
        }
        else
            t_print ("Rcvr %d is reading IQ data from: %s\n", i + 1, mcb.rcb[i].filename);

        mcb.rcb[i].rtl_read_thr = rtl_read_thr[i];
        mcb.rcb[i].hpsdrsim_sendiq_thr = hpsdrsim_sendiq_thr[i];
        mcb.rcb[i].rcvr_num = i;
        if (using_IQfiles)
        {
            r = float_malloc_align ((void **) &(mcb.rcb[i].iq_buf), 16,
                                    96000 * 2 * sizeof (float));
            //RRK mcb.output_rate * 2 * sizeof (float));
        }
        else
        {
            r = float_malloc_align ((void **) &(mcb.rcb[i].iq_buf), 16,
                                    RTL_READ_COUNT * sizeof (float));
        }

        if (r != 0)
        {
            t_print ("failed to allocate iq_buf aligned memory: r=%d\n", r);
            return (r);
        }

        if ((r = pthread_create (&rtl_read_thr[i], NULL, rtl_read_thr_func,
                                 &mcb.rcb[i])))
        {
            t_print ("error: pthread_create, r: %d\n", r);
            return (r);
        }

        if ((r = pthread_create (&hpsdrsim_sendiq_thr[i], NULL,
                                 hpsdrsim_sendiq_thr_func, &mcb.rcb[i])))
        {
            t_print ("pthread_create failed on hpsdrsim_sendiq_thr: r=%d\n", r);
            return (r);
        }
    }

    if (mcb.calibrate)
    {
        if ((r = pthread_create (&do_cal_thr, NULL, do_cal_thr_func, &mcb)))
        {
            t_print ("pthread_create failed on do_cal_thr_func: r=%d\n", r);
            return (r);
        }
    }

    if ((sock_udp = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
    {
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

    if (bind(sock_udp, (struct sockaddr *)&addr_udp, sizeof(addr_udp)) < 0)
    {
        t_perror("bind");
        return EXIT_FAILURE;
    }

    while (!do_exit)
    {
        memcpy(buffer, id, 4);
        count++;
        lenaddr = sizeof(addr_from);
        bytes_read = recvfrom(sock_udp, buffer, HPSDR_FRAME_LEN, 0, (struct sockaddr *)&addr_from, &lenaddr);

        if (bytes_read > 0)
        {
            udp_retries = 0;
        }
        else
        {
            udp_retries++;
        }

        if (bytes_read < 0 && errno != EAGAIN)
        {
            t_perror("recvfrom");
            return EXIT_FAILURE;
        }

        // If nothing has arrived via UDP for some time, try to open TCP connection.
        // "for some time" means 10 subsequent un-successful UDP rcvmmsg() calls
        if (udp_retries > 10 && oldnew != 2)
        {
            //t_print("If nothing has arrived via UDP for some time, try to open TCP connection.\n");

            // This avoids firing accept() too often if it constantly fails
            udp_retries = 0;
        }

        if (bytes_read <= 0)
        {
            continue;
        }

        count = 0;
        code = *code0;

        switch (code)
        {
        // PC to SDR transmission via process_ep2
        case 0x0201feef:

            // processing an invalid packet is too dangerous -- skip it!
            if (bytes_read != HPSDR_FRAME_LEN)
            {
                t_print("InvalidLength: RvcMsg Code=0x%08x Len=%d\n", code, (int)bytes_read);
                break;
            }

            // sequence number check
            seqnum = ((buffer[4] & 0xFF) << 24) + ((buffer[5] & 0xFF) << 16) + ((buffer[6] & 0xFF) << 8) + (buffer[7] & 0xFF);

            if (seqnum != last_seqnum + 1)
            {
                t_print("SEQ ERROR: last %ld, recvd %ld\n", (long)last_seqnum, (long)seqnum);
            }

            last_seqnum = seqnum;
            int last_running = running;
            running = 0;
            process_ep2();
            running = last_running;

            break;

        // respond to an incoming Metis detection request
        case 0x0002feef:
            if (oldnew == 2)
            {
                t_print("OldProtocol detection request from %s IGNORED.\n", inet_ntoa(addr_from.sin_addr));
                break;  // Swallow P1 detection requests
            }

            t_print( "Respond to an incoming Metis detection request from %s, code: 0x%08x\n", inet_ntoa(addr_from.sin_addr),
                     code);

            // processing an invalid packet is too dangerous -- skip it!
            if (bytes_read != 63)
            {
                t_print("InvalidLength: RvcMsg Code=0x%08x Len=%d\n", code, (int)bytes_read);
                break;
            }

            memset(buffer, 0, 60);
            buffer[0] = 0xEF;
            buffer[1] = 0xFE;
            buffer[2] = 0x02;
            buffer[3] = MAC1; // buffer[3:8] is MAC address
            buffer[4] = MAC2;
            buffer[5] = MAC3;
            buffer[6] = MAC4;
            buffer[7] = MAC5; // specifies type of radio
            buffer[8] = MAC6; // encodes old protocol
            buffer[2] = 2;

            if (new_protocol_running())
            {
                buffer[2] = 3;
            }

            buffer[9] = HERMES_FW_VER; // software version
            buffer[10] = OLDDEVICE;

            sendto(sock_udp, buffer, 60, 0, (struct sockaddr *)&addr_from, sizeof(addr_from));

            break;

        // stop the SDR to PC transmission via handler_ep6, another start will stop and
        // start the handler_ep6 thread
        case 0x0004feef:
            t_print( "STOP the transmission via handler_ep6, code: 0x%08x\n", code);

            // processing an invalid packet is too dangerous -- skip it!
            if (bytes_read != 64)
            {
                t_print("InvalidLength: RvcMsg Code=0x%08x Len=%d\n", code, bytes_read);
                break;
            }
            enable_thread = 0;

            break;

        case 0x0104feef:
        case 0x0204feef:
        case 0x0304feef:
            if (new_protocol_running())
            {
                t_print("OldProtocol START command received but NewProtocol radio already running!\n");
                break;
            }

            // processing an invalid packet is too dangerous -- skip it!
            if (bytes_read != 64)
            {
                t_print("InvalidLength: RvcMsg Code=0x%08x Len=%d\n", code, bytes_read);
                break;
            }

            t_print( "START the PC-to-SDR handler thread, code: 0x%08x\n", code);
            enable_thread = 0;

            while (active_thread)
            {
                usleep(1000);
            }

            memset(&addr_old, 0, sizeof(addr_old));
            addr_old.sin_family = AF_INET;
            addr_old.sin_addr.s_addr = addr_from.sin_addr.s_addr;
            addr_old.sin_port = addr_from.sin_port;
            enable_thread = 1;
            active_thread = 1;
            //running = 1;

            if (pthread_create(&thread, NULL, handler_ep6, NULL) < 0)
            {
                t_perror("create old protocol thread");
                return EXIT_FAILURE;
            }

            pthread_detach(thread);
            break;

        default:

            /*
             * Here we have to handle the following "non standard" cases:
             * NewProtocol "Discovery" packet   60 bytes starting with 00 00 00 00 02
             * NewProtocol "General"   packet   60 bytes starting with 00 00 00 00 00
             *                                  ==> this starts NewProtocol radio
             */
            if (code == 0 && buffer[4] == 0x02)
            {
                if (oldnew == 1)
                {
                    t_print("NewProtocol discovery packet from %s IGNORED.\n", inet_ntoa(addr_from.sin_addr));
                    break;
                }

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
                buffer[11] = NEWDEVICE;
                buffer[12] = 38;
                buffer[13] = 19;
                buffer[20] = 2;
                buffer[21] = 1;
                buffer[22] = 3;

                sendto(sock_udp, buffer, 60, 0, (struct sockaddr *)&addr_from, sizeof(addr_from));
                break;
            }

            if (bytes_read == 60 && buffer[4] == 0x00)
            {
                if (oldnew == 1)
                {
                    t_print("NewProtocol General packet IGNORED.\n");
                    break;
                }

                // handle "general packet" of the new protocol
                memset(&addr_new, 0, sizeof(addr_new));
                addr_new.sin_family = AF_INET;
                addr_new.sin_addr.s_addr = addr_from.sin_addr.s_addr;
                addr_new.sin_port = addr_from.sin_port;
                //new_protocol_general_packet(buffer);
                break;
            }
            else
            {
                t_print("Invalid packet (len=%d) detected: ", bytes_read);

                for (i = 0; i < 16; i++)
                {
                    t_print("%02x ", buffer[i]);
                }

                t_print("\n");
            }

            break;
        }
    }

    close(sock_udp);

    return EXIT_SUCCESS;
}

void process_ep2(int frame)
{
    int ep, bytes_read = 0;
    int i = 0, j = 0;
    int rc, offset;
    int freq, num_rcvrs;
    u_char C0_1, C0_2;

#if 0
    for (i = 0; i < HPSDR_FRAME_LEN; i++)
    {
        t_print ("%4d:%2x ", i, buffer[i]);

        if (!((i + 1) % 8))
            t_print ("\n");
    }
#endif
    // REMEMBER, 2 USB packets of data here (audio, C0, ...)
    C0_1 = buffer[11] & 0xFE;
    C0_2 = buffer[523] & 0xFE;
    freq = 0;

    if ((C0_1 >= 4)
            && (C0_1 <= (4 + ((mcb.total_num_rcvrs - 1) * 2))))
    {
        offset = 0;
        freq = 1;
        j = (C0_1 - 4) / 2;
    }
    else if ((C0_2 >= 4)
             && (C0_2 <=
                 (4 + ((mcb.total_num_rcvrs - 1) * 2))))
    {
        offset = 512;
        freq = 1;
        j = (C0_2 - 4) / 2;
    }

    if (freq)
    {
        freq =
            (int) buffer[12 + offset] << 24 | (int) buffer[13 +
                    offset]
            << 16 | (int) buffer[14 +
                                 offset] << 8 | (int) buffer[15 +
                                         offset];

        if (last_freq[j] != freq && 0 != freq)
        {
            if (common_freq)
            {
                for (i = 0; i < mcb.active_num_rcvrs; i++)
                {
                    mcb.rcb[i].new_freq = freq;
                }
            }
            else if (j < mcb.total_num_rcvrs)
            {
                mcb.rcb[j].new_freq = freq;
            }
            last_freq[j] = freq;
            ftime (&mcb.freq_ltime[j]);
        }
    }

    if ((C0_1 == 0x00) || (C0_2 == 0x00))
    {
        offset = (C0_1 == 0x00) ? 0 : 512;

        if (last_rate != (buffer[12 + offset] & 3))
        {
            last_rate = (buffer[12 + offset] & 3);

            switch (last_rate)
            {
            case 3:
                mcb.output_rate = 384000;
                break;

            case 2:
                mcb.output_rate = 192000;
                break;

            case 1:
                mcb.output_rate = 96000;
                break;

            case 0:
                mcb.output_rate = 48000;
                break;

            default:
                t_print ("WARNING: UNSUPPORTED RATE: %x!!!\n",
                        last_rate);
            }

            t_print ("Setting hpsdr output rate to %d hz\n",
                    mcb.output_rate);
        }

        if (last_num_rcvrs != (buffer[15 + offset] & 0x38))
        {
            last_num_rcvrs = (buffer[15 + offset] & 0x38);
            num_rcvrs = (last_num_rcvrs >> 3) + 1;

            if (num_rcvrs > MAX_RCVRS)
            {
                t_print ("ERROR: Attempt to exceed max number of rcvrs: %d\n",
                 MAX_RCVRS);
                hpsdrsim_stop_threads ();
                exit (-1);
            }

            if (num_rcvrs <= mcb.total_num_rcvrs)
            {
                num_copy_rcvrs = 0;
                mcb.active_num_rcvrs = num_rcvrs;
            }
            else
            {
                num_copy_rcvrs =
                    num_rcvrs - mcb.total_num_rcvrs;
                mcb.active_num_rcvrs = mcb.total_num_rcvrs;
            }

            mcb.nsamps_packet =
                nsamps_packet[mcb.active_num_rcvrs - 1];
            mcb.frame_offset1 =
                frame_offset1[mcb.active_num_rcvrs - 1];
            mcb.frame_offset2 =
                frame_offset2[mcb.active_num_rcvrs - 1];
            mcb.rcvrs_mask = 1;


            // disable all previous rcvrs except rcvr 1
            for (i = 1; i < mcb.total_num_rcvrs; i++)
            {
                mcb.rcb[i].rcvr_mask = 0;
            }

            // now enable any new ones
            for (i = 1; i < mcb.active_num_rcvrs; i++)
            {
                mcb.rcvrs_mask |= 1 << i;
                mcb.rcb[i].rcvr_mask = 1 << i;
                mcb.rcb[i].output_rate = 0;
            }
            cal_rcvr_mask = mcb.rcvrs_mask;

            t_print ("Requested %d Activated %d actual rcvr(s)\n",
             num_rcvrs, mcb.active_num_rcvrs);

            if (num_copy_rcvrs > 0)
            {
                for (i = mcb.active_num_rcvrs; i < num_rcvrs;
                        i++)
                {
                    copy_rcvr[i] = i;
                }

                t_print ("Activated %d COPY(S) of rcvr %d\n",
                        num_copy_rcvrs, mcb.active_num_rcvrs);
            }
        }
        common_freq = (buffer[15] & 0x80) ? 1 : 0;
    }

    // handle the audio data if we assigned an audio device
    if (mcb.sound_dev[0])
        write_local_sound (&(buffer[8]));

}

void *handler_ep6(void *arg)
{
    uint32_t counter = 0;

    //t_print("start handler_ep6()\n");
    while (1)
    {
        running = 0;
        if (!enable_thread)
        {
            break;
        }

        running = 1;
        // wait for all rcvrs to have formatted data ready before sending
        pthread_mutex_lock (&send_lock);
        while (send_flags != mcb.rcvrs_mask)
        {
            pthread_cond_wait (&send_cond, &send_lock);
        }

#if 0                           // dump the frame for analysis
        if (iloop++ == 100)
        {
            iloop = 0;
            t_print ("rcvrs_mask:%x send_flags:%d\n", mcb.rcvrs_mask,
                    send_flags);

            for (i = 0; i < HPSDR_FRAME_LEN; i++)
            {
                t_print ("%4d:%2x ", i, payload[i]);

                if (!((i + 1) % 8))
                    t_print ("\n");
            }
        }

//                      exit(0);
#endif
        // plug in sequence numbers
        *(uint32_t *)(payload + 4) = htonl(counter);
        ++counter;

        pthread_mutex_lock (&done_send_lock);

        sendto(sock_udp, payload, HPSDR_FRAME_LEN, 0, (struct sockaddr *)&addr_old, sizeof(addr_old));
        pthread_mutex_unlock (&send_lock);
        send_flags = 0;
        pthread_cond_broadcast (&done_send_cond);
        pthread_mutex_unlock (&done_send_lock);
    }

    active_thread = 0;
    running = 0;
    //t_print("stop handler_ep6()\n");
    return NULL;
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

    if (first)
    {
        first = 0;
        starttime = now;
    }

    //
    // After 11 days, the time reaches 999999.999 so we simply wrap around
    //
    if (now - starttime >= 999999.995)
    {
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
