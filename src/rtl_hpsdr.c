/*	This file rtl_hpsdr.c is part of rtl_hpsdr.
 *
 *	rtl_hpsdr - an RTL to HPSDR software translation server
 *	Copyright (C) 2014 Richard Koch
 *
 *	rtl_hpsdr is free software: you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation, either version 3 of the License, or
 *	(at your option) any later version.
 *
 *	rtl_hpsdr is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *	GNU General Public License for more details.
 *
 *	You should have received a copy of the GNU General Public License
 *	along with rtl_hpsdr.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "rtl_hpsdr.h"
#include "convenience/convenience.h"
#include "version.h"
#include <sched.h>
#include <unistd.h>
#include <sys/stat.h>

#ifdef _WIN32
#include "getopt/getopt.h"
#pragma comment(lib, "IPHLPAPI.lib")
#pragma comment(lib, "ws2_32.lib")
typedef int socklen_t;

#else
#define closesocket close
#define SOCKADDR struct sockaddr
#define SOCKET int
#define SOCKET_ERROR -1
#endif

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
static float dcIlast[MAX_RCVRS] = { 0.0f }, dcQlast[MAX_RCVRS] = {
0.0f};

static int common_freq = 0;

static int reveal_socket;
static struct sockaddr_in my_addr;
static socklen_t my_length;
static struct sockaddr_in their_addr;
static socklen_t their_length;

static int nsamps_packet[8] = { 126, 72, 50, 38, 30, 26, 22, 20 };
static int frame_offset1[8] = { 520, 520, 516, 510, 496, 510, 500, 516 };
static int frame_offset2[8] =
   { 1032, 1032, 1028, 1022, 1008, 1022, 1012, 1028 };

static int revealed = 0;
static int running = 0;
static char conf_file[MAXSTR];

static u_char hw_address[6];
static char server_ip_address[16] = {0,};

static u_char buffer[MAX_BUFFER_LEN];
static u_char payload[HPSDR_FRAME_LEN];

static u_int hpsdr_sequence = 0;
static u_int pc_sequence;
static u_int network_error = 0;

static float rtl_lut[4][256];
static u_char *fft_buf;
static int reset_cal = 0;
static int do_cal_time = 5;	//seconds between calibration
static bool using_IQfiles = false, using_IQrewind = false;

// using clock_nanosleep of librt
extern int clock_nanosleep(clockid_t __clock_id, int __flags,
      __const struct timespec *__req,
      struct timespec *__rem);

void
rtl_sighandler (int signum)
{
   printf ("Signal caught, exiting!\n");
   do_exit = 1;
   if (running)
      running = 0;
   else
      mcb.calibrate = 0;
   hpsdrsim_stop_threads ();
#ifdef _WIN32
   exit (0);			// RRK FIXME
#endif
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

#ifdef _WIN32
int
get_addr (int sock)
{
   struct sockaddr_in client_info = { 0 };
   socklen_t client_info_len = sizeof (client_info);
   IP_ADAPTER_INFO AdapterInfo[16];
   PIP_ADAPTER_INFO pAdapterInfo;
   int i, addrsize = sizeof (client_info);
   char *ip;

   DWORD dwBufLen = sizeof (AdapterInfo);

   DWORD dwStatus = GetAdaptersInfo (AdapterInfo, &dwBufLen);

   pAdapterInfo = AdapterInfo;

   do {
      if (!strcmp (pAdapterInfo->IpAddressList.IpAddress.String, mcb.ip_addr)) {
	 for (i = 0; i < 6; i++)
	    hw_address[i] = pAdapterInfo->Address[i];
      }
      pAdapterInfo = pAdapterInfo->Next;
   }
   while (pAdapterInfo);

   return 0;
}

int
float_malloc_align (void **voidptr, int alignment, int bytes)
{
   *voidptr = _aligned_malloc (bytes, alignment);
   if (*voidptr == NULL)
      return 1;
   else
      return 0;
}

void
float_free_align (float *ptr)
{
   _aligned_free ((void *) ptr);
}

#else

int
get_addr (int sock)
{

   struct ifreq *ifr;
   struct ifconf ifc;
   unsigned char *u;
   char buf[1024];
   int i, j, k;

   // query available interfaces
   ifc.ifc_len = sizeof (buf);
   ifc.ifc_buf = buf;
   if (ioctl (sock, SIOCGIFCONF, &ifc) < 0) {
      printf ("Error: ioctl(SIOCGIFCONF)");
      return -1;
   }

   ifr = ifc.ifc_req;
   j = ifc.ifc_len / sizeof (struct ifreq);
   for (i = 0; i < j; i++) {
      struct ifreq *item = &ifr[i];

      if (!(strcmp (mcb.ip_addr,
		    inet_ntoa (((struct sockaddr_in *) &item->ifr_addr)->
			       sin_addr)))) {
	 // get the MAC address
	 if (ioctl (sock, SIOCGIFHWADDR, item) < 0) {
	    printf ("Error: ioctl(SIOCGIFHWADDR)");
	    return -1;
	 }
	 u = (unsigned char *) &item->ifr_addr.sa_data;

	 for (k = 0; k < 6; k++)
	    hw_address[k] = u[k];

	 break;
      }
   }

   if (i >= j) {
      printf ("Error: no interface found for ip %s", mcb.ip_addr);
      return -1;
   }

   return 0;
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
#endif

void
hpsdrsim_reveal (void)
{
   int rc, bytes_read;
   u_int i, on = 1;
   bool ready = false;
   u_char init_buffer[19];

   if (mcb.rtl_mode == RTL_MODE_SKIMMER) {
     init_buffer[0] = 0xEF;
     init_buffer[1] = 0xFE;
     init_buffer[2] = (2 + running);
     init_buffer[3] = 0;
     init_buffer[4] = 0;
     init_buffer[5] = 0;
     init_buffer[6] = 0;
     init_buffer[7] = 0;
     init_buffer[8] = 0; 
     init_buffer[9] = HERMES_FW_VER;
     init_buffer[10] = 1;
     init_buffer[11] = 'R';
     init_buffer[12] = 'T';
     init_buffer[13] = 'L';
     init_buffer[14] = '_';
     init_buffer[15] = 'N';
     init_buffer[16] = '1';
     init_buffer[17] = 'G';
     init_buffer[18] = 'P';

     printf ("Revealing myself as a Hermes version %1.1f rcvr. -- Skimmer Mode\n",
	     (float) HERMES_FW_VER / 10.0);
   }
   else {
     init_buffer[0] = 0xEF;
     init_buffer[1] = 0xFE;
     init_buffer[2] = (2 + running);
     init_buffer[3] = 0;
     init_buffer[4] = 0;
     init_buffer[5] = 0;
     init_buffer[6] = 0;
     init_buffer[7] = 0;
     init_buffer[8] = 0; 
     init_buffer[9] = HERMES_FW_VER;
     init_buffer[10] = 6;
     init_buffer[11] = 'H';
     init_buffer[12] = 'E';
     init_buffer[13] = 'R';
     init_buffer[14] = 'M';
     init_buffer[15] = 'E';
     init_buffer[16] = 'S';
     init_buffer[17] = 'L';
     init_buffer[18] = 'T';

     printf ("Revealing myself as a Hermes LT version %1.1f rcvr. -- WSPR Mode\n",
	     (float) HERMES_FW_VER / 10.0);
   }

   reveal_socket = socket (PF_INET, SOCK_DGRAM, IPPROTO_UDP);

   if (reveal_socket < 0) {
      perror ("create socket failed for reveal_socket\n");
      exit (1);
   }

   rc =
      setsockopt (reveal_socket, SOL_SOCKET, SO_REUSEADDR, (char *) &on,
		  sizeof (on));

   if (rc != 0) {
      printf ("cannot set SO_REUSEADDR: rc=%d\n", rc);
      exit (1);
   }

   // bind to this interface
   my_addr.sin_family = AF_INET;
   my_addr.sin_port = htons (PORT);
   my_addr.sin_addr.s_addr = htonl (INADDR_ANY);

   if (bind (reveal_socket, (struct sockaddr *) &my_addr, sizeof (my_addr)) <
       0) {
      perror ("1 bind socket failed for reveal_socket\n");
      exit (1);
   }

   // allow broadcast on the socket
   rc =
      setsockopt (reveal_socket, SOL_SOCKET, SO_BROADCAST, (char *) &on,
		  sizeof (on));

   if (rc != 0) {
      printf ("cannot set SO_BROADCAST: rc=%d\n", rc);
      exit (1);
   }

   // get my MAC address
   if (server_ip_address[0] == 0) {
      if (get_addr (reveal_socket) < 0) {
         exit (1);
      }
   }

   printf ("My IP Address: %s\n", mcb.ip_addr);
   printf ("My MAC Address: %02x:%02x:%02x:%02x:%02x:%02x\n",
	   hw_address[0], hw_address[1], hw_address[2], hw_address[3],
	   hw_address[4], hw_address[5]);

   for (i = 0; i < 6; i++)
      init_buffer[3 + i] = hw_address[i];

   their_length = sizeof (their_addr);

   memset (&their_addr, 0, their_length);
   their_addr.sin_family = AF_INET;
   their_addr.sin_port = htons (PORT);
   their_addr.sin_addr.s_addr = htonl (INADDR_BROADCAST);

   // Get discovered
   while (!ready) {
      if ((bytes_read = recvfrom (reveal_socket, buffer, sizeof (buffer), 0,
				  (struct sockaddr *) &their_addr,
				  &their_length)) < 0) {
	 if (!do_exit)
	    printf ("Bad recvfrom, NOT discovered!");

	 exit (0);
      }				// else printf("Received %d bytes, waiting to be discovered...\n", bytes_read);

      // the discovery process is complete, continue on
      if ((bytes_read == 64 || bytes_read > 1000) && revealed) {
	 ready = true;
	 continue;
      }

      if (buffer[0] == 0xEF && buffer[1] == 0xFE && buffer[2] == 0x02) {
	 strcpy (server_ip_address, inet_ntoa (their_addr.sin_addr));
	 printf ("Was discovered by %s\n", server_ip_address);

	 // Send acknowledgement of discovery
	 for (i = 0; i < sizeof (init_buffer); i++) {
	    buffer[i] = init_buffer[i];
	 }

	 // the 49 trailing bytes are undefined, HermesIntf.dll
	 // will use the RTL_N1GP emulation ID and the dongle count
	 buffer[sizeof (init_buffer)] = mcb.total_num_rcvrs;

	 for (i = sizeof (init_buffer) + 1; i < 60; i++) {
	    buffer[i] = 1;
	 }

	 if (sendto
	     (reveal_socket, buffer, 60, 0, (struct sockaddr *) &their_addr,
	      sizeof (their_addr)) < 0) {
	    perror ("sendto() failed sending acknowledgement!\n");
	    exit (1);
	 }
	 else
	    printf ("Sent discovery acknowledgement.\n");

	 revealed = 1;
	 close (reveal_socket);
	 reveal_socket = socket (AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	 my_length = sizeof (my_addr);
	 memset (&my_addr, 0, my_length);
	 my_addr.sin_family = AF_INET;
	 my_addr.sin_port = htons (PORT);
	 my_addr.sin_addr.s_addr = inet_addr (mcb.ip_addr);
	 rc =
	    setsockopt (reveal_socket, SOL_SOCKET, SO_REUSEADDR, (char *) &on,
			sizeof (on));
	 if (rc != 0) {
	    printf ("cannot set SO_REUSEADDR: rc=%d\n", rc);
	    exit (1);
	 }
#if 0				// resize buffer
	 int len, trysize, gotsize;

	 len = sizeof (int);
	 trysize = 1048576 + 32768;
	 do {
	    trysize -= 32768;
	    setsockopt (reveal_socket, SOL_SOCKET, SO_SNDBUF,
			(char *) &trysize, len);
	    rc =
	       getsockopt (reveal_socket, SOL_SOCKET, SO_SNDBUF,
			   (char *) &gotsize, &len);
	    if (rc < 0) {
	       perror ("getsockopt");
	       break;
	    }
	 } while (gotsize < trysize);
	 printf ("Size set to %d\n", gotsize);
#endif

	 if (bind (reveal_socket, (struct sockaddr *) &my_addr, my_length) <
	     0) {
	    perror ("3 bind socket failed for reveal_socket\n");
	    exit (1);
	 }
      }
      else {
	 //printf("Was NOT discovered by %s\n", inet_ntoa(their_addr.sin_addr));
      }
   }

   rc = pthread_create (&hpsdrsim_thread_id, NULL, hpsdrsim_thread, NULL);

   if (rc != 0) {
      printf ("pthread_create failed on hpsdr_thread: rc=%d\n", rc);
      exit (1);
   }
}

void
load_packet (struct rcvr_cb *rcb)
{
   int b, i, j, k, copy_total = (mcb.active_num_rcvrs - 1) + num_copy_rcvrs;

   // if we need to copy a receiver we'll choose the last active 'real' one
   bool do_copy = ((num_copy_rcvrs > 0)
		   && (rcb->rcvr_num == mcb.active_num_rcvrs - 1));
   int offset1 =
      (num_copy_rcvrs > 0) ? frame_offset1[copy_total] : mcb.frame_offset1;
   int offset2 =
      (num_copy_rcvrs > 0) ? frame_offset2[copy_total] : mcb.frame_offset2;
   int offsetx;
   float *out_buf = &rcb->iqSamples[rcb->iqSample_offset * 2];
   int IQData;

   i = 0;
   k = 0;
   b = 16;

   pthread_mutex_lock (&send_lock);

   // insert data in lower and upper bank for each of the receivers
   while (k++ < 2) {
      offsetx = (i == 0) ? offset1 : offset2;
      while (b < offsetx) {
	 for (j = 0; j < mcb.active_num_rcvrs + num_copy_rcvrs; j++) {
	    if ((j == rcb->rcvr_num) || (do_copy && copy_rcvr[j] == j)) {
	       if (using_IQfiles) {
	           IQData = ((int)(out_buf[i]));
	           payload[b++] = (IQData & 0xff00) >> 8;
	           payload[b++] = (IQData & 0xff0000) >> 16;
	           payload[b++] = (IQData & 0xff000000) >> 24;
	           IQData = ((int)(out_buf[i+1]));
	           payload[b++] = (IQData & 0xff00) >> 8;
	           payload[b++] = (IQData & 0xff0000) >> 16;
	           payload[b++] = (IQData & 0xff000000) >> 24;
           } else {
	           IQData = (int) out_buf[i];
	           payload[b++] = IQData >> 8;
	           payload[b++] = IQData & 0xff;
	           b++;
	           IQData = (int) out_buf[i + 1];
	           payload[b++] = IQData >> 8;
	           payload[b++] = IQData & 0xff;
	           b++;
           }

	       if (do_copy) {
		  if (j == copy_total)
		     i += 2;
	       }
	       else {
		  i += 2;
	       }
	    }
	    else {
	       b += 6;
	    }
	 }
	 b += 2;		// skip mic data
      }
      b = 528;
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

void *
hpsdrsim_sendiq_thr_func (void *arg)
{
  int samps_packet, i;
  char num[16];
  register int rxnum;

   struct rcvr_cb *rcb = (struct rcvr_cb *) arg;

   rcb->iqSample_offset = rcb->iqSamples_remaining = 0;

   //printf("ENTERING hpsdrsim_sendiq_thr_func() rcvr %d...\n", rcb->rcvr_num+1);
   while (!do_exit) {
      if (!running) rcb->iqSample_offset = rcb->iqSamples_remaining = 0;
      samps_packet = (num_copy_rcvrs > 0)
	 ? nsamps_packet[(mcb.active_num_rcvrs - 1) + num_copy_rcvrs]
	 : mcb.nsamps_packet;

      pthread_mutex_lock (&iqready_lock);
      while (!(rcvr_flags & rcb->rcvr_mask)) {
	 pthread_cond_wait (&iqready_cond, &iqready_lock);
      }
      rcvr_flags &= ~rcb->rcvr_mask;
      pthread_mutex_unlock (&iqready_lock);

      // can happen when switching between rcvr numbers
      if (rcb->iqSamples_remaining < 0)
	 rcb->iqSamples_remaining = 0;

      if (!using_IQfiles) {
      // downsample starting at any remaining offset
      downsample (rcb);

      switch (mcb.output_rate) {
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
      } else {
           memcpy(&rcb->iqSamples[rcb->iqSamples_remaining * 2], &rcb->iq_buf[0], 9600 * 2 * sizeof (float));
	   //RRK rcb->iqSamples_remaining += mcb.output_rate * 2;
	   rcb->iqSamples_remaining += 9600;
      }

      while (rcb->iqSamples_remaining > samps_packet) {
	 load_packet (rcb);
	 rcb->iqSamples_remaining -= samps_packet;
	 rcb->iqSample_offset += samps_packet;
      }

      // move remaining samples to beginning of buffer
      if ((rcb->iqSample_offset > 0) && (rcb->iqSamples_remaining > 0)) {
	 memcpy (&(rcb->iqSamples[0]),
		 &(rcb->iqSamples[rcb->iqSample_offset * 2]),
		 rcb->iqSamples_remaining * 2 * sizeof (float));
	 rcb->iqSample_offset = 0;
      }

      // Set new frequency if one is pending and enough time has expired
      if (!using_IQfiles) {
        if ((mcb.rcb[rcb->rcvr_num].new_freq) && (mcb.rcb[rcb->rcvr_num].new_freq != 10000000)) {
	   ftime (&mcb.freq_ttime[rcb->rcvr_num]);
	   if ((((((mcb.freq_ttime[rcb->rcvr_num].time * 1000) +
		 mcb.freq_ttime[rcb->rcvr_num].millitm) -
		(mcb.freq_ltime[rcb->rcvr_num].time * 1000) +
		mcb.freq_ltime[rcb->rcvr_num].millitm)) > 200)
	     || hpsdr_sequence < 1000000) {
	    i = mcb.rcb[rcb->rcvr_num].new_freq + mcb.up_xtal +
	       mcb.freq_offset[rcb->rcvr_num];
	    i = rtlsdr_set_center_freq (mcb.rcb[rcb->rcvr_num].rtldev, i);
	    if (i < 0) {
	       printf
		  ("WARNING: Failed to set rcvr %d to freq %d with offset %+d\n",
		   mcb.rcb[rcb->rcvr_num].rcvr_num + 1,
		   mcb.rcb[rcb->rcvr_num].new_freq,
		   mcb.freq_offset[rcb->rcvr_num]);
	    }

	    mcb.rcb[rcb->rcvr_num].curr_freq = mcb.rcb[rcb->rcvr_num].new_freq;
	    mcb.rcb[rcb->rcvr_num].new_freq = 0;

#if 1
            rxnum = rcb->rcvr_num;
            printf
              ("INFO: Rx[%d]: if_bw: %dHz, offset: %dHz, center: %dHz [%8dHz], gain: %0.2f dB, gain_mode: %s, agc: %s, bias_t: %s, direct: %s\n",
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
   //printf("EXITING hpsdrsim_sendiq_thr_func() rcvr_mask%d...\n", rcb->rcvr_mask);
}

void *
hpsdrsim_thread (void *arg)
{
   int ep, bytes_read = 0;
   int i = 0, j = 0;
   int rc, offset;
   int freq, num_rcvrs, xtra;
   u_char C0_1, C0_2;

   //printf("ENTERING hpsdrsim_thread active rcvrs: %d\n", mcb.active_num_rcvrs);

   // start a watchdog to make sure we are sending frames
   rc = pthread_create (&watchdog_thread_id, NULL,
			hpsdrsim_watchdog_thread, NULL);

   if (rc != 0) {
      printf ("pthread_create failed on hpsdrsim_watchdog_thread: rc=%d\n", rc);
      exit (1);
   }

   rc = pthread_create (&discovery_thread_id, NULL,
			hpsdrsim_discovery_thread, NULL);

   if (rc != 0) {
      printf ("pthread_create failed on discovery_thread: rc=%d\n", rc);
      exit (1);
   }

   while (!do_exit) {
      if (buffer[0] == 0xEF && buffer[1] == 0xFE) {
	 switch (buffer[2]) {
	   case 1:
	      // get the end point
	      ep = buffer[3] & 0xFF;

	      // for some reason cuSDR 3.2.13 sends command
	      // packets by an extra 4 bytes, cuSDR 3.2.14 is OK
	      xtra =
		 ((buffer[8] == 0x7f && buffer[9] == 0x7f
		   && buffer[10] == 0x7f)) ? 0 : 4;

	      switch (ep) {
		case 6:
		   printf ("EP6 data\n");
		   break;

		case 4:
		   printf ("EP4 data\n");
		   break;

		case 2:
		   // REMEMBER, 2 USB packets of data here (audio, C0, ...)
		   //printf("EP2 data\n");
		   // get the pc_sequence number
		   //pc_sequence=((buffer[4]&0xFF)<<24)+((buffer[5]&0xFF)<<16)+((buffer[6]&0xFF)<<8)+(buffer[7]&0xFF);
		   //printf("Received data ep=%d pc_sequence=%d\n",ep,pc_sequence);
		   C0_1 = buffer[11 + xtra] & 0xFE;
		   C0_2 = buffer[523 + xtra] & 0xFE;
		   freq = 0;

		   if ((C0_1 >= 4)
		       && (C0_1 <= (4 + ((mcb.total_num_rcvrs - 1) * 2)))) {
		      offset = xtra;
		      freq = 1;
		      j = (C0_1 - 4) / 2;
		   }
		   else if ((C0_2 >= 4)
			    && (C0_2 <=
				(4 + ((mcb.total_num_rcvrs - 1) * 2)))) {
		      offset = 512 + xtra;
		      freq = 1;
		      j = (C0_2 - 4) / 2;
		   }

		   if (freq) {
		      freq =
			 (int) buffer[12 + offset] << 24 | (int) buffer[13 +
									offset]
			 << 16 | (int) buffer[14 +
					      offset] << 8 | (int) buffer[15 +
									  offset];

		      if (last_freq[j] != freq && 0 != freq) {

			 if (common_freq) {
			    for (i = 0; i < mcb.active_num_rcvrs; i++) {
			       mcb.rcb[i].new_freq = freq;
			    }
			 }
			 else if (j < mcb.total_num_rcvrs) {
			    mcb.rcb[j].new_freq = freq;
			 }
			 last_freq[j] = freq;
			 ftime (&mcb.freq_ltime[j]);
		      }
		   }

		   if ((C0_1 == 0x00) || (C0_2 == 0x00)) {
		      offset = (C0_1 == 0x00) ? xtra : 512 + xtra;

		      if (last_rate != (buffer[12 + offset] & 3)) {
			 last_rate = (buffer[12 + offset] & 3);

			 switch (last_rate) {
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
			      printf ("WARNING: UNSUPPORTED RATE: %x!!!\n",
				      last_rate);
			 }

			 printf ("Setting hpsdr output rate to %d hz\n",
				 mcb.output_rate);
		      }

		      if (last_num_rcvrs != (buffer[15 + offset] & 0x38)) {
			 last_num_rcvrs = (buffer[15 + offset] & 0x38);
			 num_rcvrs = (last_num_rcvrs >> 3) + 1;

			 if (num_rcvrs > MAX_RCVRS) {
			    printf
			       ("ERROR: Attempt to exceed max number of rcvrs: %d\n",
				MAX_RCVRS);
			    hpsdrsim_stop_threads ();
			    exit (-1);
			 }
			 else if (num_rcvrs > 1) {
			    if (num_rcvrs <= mcb.total_num_rcvrs) {
			       num_copy_rcvrs = 0;
			       mcb.active_num_rcvrs = num_rcvrs;
			    }
			    else {
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
			    for (i = 1; i < mcb.total_num_rcvrs; i++) {
			       mcb.rcb[i].rcvr_mask = 0;
			    }

			    // now enable any new ones
			    for (i = 1; i < mcb.active_num_rcvrs; i++) {
			       mcb.rcvrs_mask |= 1 << i;
			       mcb.rcb[i].rcvr_mask = 1 << i;
			       mcb.rcb[i].output_rate = 0;
			    }
			    cal_rcvr_mask = mcb.rcvrs_mask;

			    printf
			       ("Requested %d Activated %d actual rcvr(s)\n",
				num_rcvrs, mcb.active_num_rcvrs);

			    if (num_copy_rcvrs > 0) {
			       for (i = mcb.active_num_rcvrs; i < num_rcvrs;
				    i++) {
				  copy_rcvr[i] = i;
			       }

			       printf ("Activated %d COPY(S) of rcvr %d\n",
				       num_copy_rcvrs, mcb.active_num_rcvrs);
			    }
			 }
		      }
		      common_freq = (buffer[15 + xtra] & 0x80) ? 1 : 0;
		   }

#ifndef _WIN32
		   // handle the audio data if we assigned an audio device
		   if (mcb.sound_dev[0])
		      write_local_sound (&(buffer[8 + xtra]));
#endif

		   break;

		default:
		   printf ("unexpected EP %d\n", ep);
		   break;
	      }

	      break;

	   case 2:
	      //ignore
	      break;

	   case 4:		// start / stop command
	      if (buffer[3] & 1) {
		 printf ("Received Start command\n");
		 running = 1;
	      }
	      else {
		 printf ("Received Stop command\n");
		 running = 0;
		 hpsdr_sequence = 0;
	      }

	      break;

	   default:
	      printf ("unexpected packet type: 0x%02X\n", buffer[2]);
	      break;
	 }
      }
      else {
	 printf ("Received bad header bytes on data port %02X,%02X\n",
		 buffer[0], buffer[1]);
      }

      bytes_read = recvfrom (reveal_socket, buffer, sizeof (buffer), 0,
			     (struct sockaddr *) &my_addr, &my_length);

      if (bytes_read < 0) {
	 perror ("recvfrom socket failed for hpsdrsim_thread\n");
	 exit (1);
      }				//else printf("hpsdrsim_thread, RECV'D %d bytes\n", bytes_read); //1036 cuSDR64, 1032 ghpsdr3
   }

   //printf("EXITING hpsdrsim_thread()\n");
   pthread_exit (NULL);
}

void
hpsdrsim_stop_threads ()
{
   int i;

   revealed = running = 0;

   for (i = 0; i < mcb.total_num_rcvrs; i++) {
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

   pthread_cancel (watchdog_thread_id);
   pthread_cancel (discovery_thread_id);
   pthread_cancel (hpsdrsim_thread_id);

   if (mcb.calibrate) {
      printf ("\nfreq_offset ");
      for (i = 0; i < mcb.active_num_rcvrs; i++)
	 printf ("%d%s", mcb.freq_offset[i],
		 (mcb.active_num_rcvrs - 1 != i) ? "," : "\n\n");
   }
}


void update_config()
{
    int i, r;
    for (i = 0; i < mcb.total_num_rcvrs; i++) {

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

    printf("\n");

    for (i = 0; i < mcb.total_num_rcvrs; i++) {

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
	if (r < 0) {
	    printf ("WARNING: Failed to set tuner freq to %dhz!\n",
		    mcb.center_freq[i]);
	    return (-1);
	}

	r = rtlsdr_set_direct_sampling (rtldev, mcb.direct_mode[i]);
	if (r < 0) {
	    printf ("WARNING: Failed to set direct sampling!\n");
	    return (-1);
	}

	r = rtlsdr_set_agc_mode (rtldev, mcb.agc_mode[i]);
	if (r < 0) {
	    printf ("WARNING: Failed to set automatic gain!\n");
	    return (-1);
	}

	r = rtlsdr_set_bias_tee (rtldev, mcb.bias_t[i]);
	if (r < 0) {
	    printf ("WARNING: Failed to set bias T!\n");
	    return (-1);
	}

	sprintf (num, "%d", mcb.gain[i]);

#if 1
	//	gain = mcb.gain[i];

	printf
	  ("INFO: Rx[%d]: if_bw: %dHz, center: %dHz [%8dHz], gain: %0.2f dB, gain_mode: %s, agc: %s, bias_t: %s, direct: %s\n",
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

// RRK TODO, make a unified discovery thread
void *
hpsdrsim_discovery_thread (void *arg)
{
   u_char ibuffer[MAX_BUFFER_LEN];
   int rc, bytes_read, on = 1;
   int discovery_socket;
   struct sockaddr_in my_addr2;
   socklen_t my_length2;
   struct sockaddr_in their_addr2;
   socklen_t their_length2;

  //printf("START hpsdrsim_discovery_thread\n");

   discovery_socket = socket (PF_INET, SOCK_DGRAM, IPPROTO_UDP);

   if (discovery_socket < 0) {
      perror ("create socket failed for discovery_socket\n");
      exit (1);
   }

   rc =
      setsockopt (discovery_socket, SOL_SOCKET, SO_REUSEADDR, (char *) &on,
		  sizeof (on));

   if (rc != 0) {
      printf ("cannot set SO_REUSEADDR: rc=%d\n", rc);
      exit (1);
   }

   // bind to this interface
   my_addr2.sin_family = AF_INET;
   my_addr2.sin_port = htons (PORT);
   my_addr2.sin_addr.s_addr = htonl (INADDR_ANY);

   if (bind (discovery_socket, (struct sockaddr *) &my_addr2, sizeof (my_addr2)) <
       0) {
      perror ("1 bind socket failed for discovery_socket\n");
      exit (1);
   }

   // allow broadcast on the socket
   rc =
      setsockopt (discovery_socket, SOL_SOCKET, SO_BROADCAST, (char *) &on,
		  sizeof (on));

   if (rc != 0) {
      printf ("cannot set SO_BROADCAST: rc=%d\n", rc);
      exit (1);
   }

   their_length2 = sizeof (their_addr2);

   memset (&their_addr2, 0, their_length2);
   their_addr2.sin_family = AF_INET;
   their_addr2.sin_port = htons (PORT);
   while (1) {
      if ((bytes_read = recvfrom (discovery_socket, ibuffer, sizeof (ibuffer), 0,
				  (struct sockaddr *) &their_addr2,
				  &their_length2)) < 0) {
      }				// else printf("Received %d bytes, waiting to be discovered...\n", bytes_read);

      if (ibuffer[0] == 0xEF && ibuffer[1] == 0xFE && ibuffer[2] == 0x02) {
	 if (!strcmp (server_ip_address, inet_ntoa (their_addr2.sin_addr))) {
	     printf ("Was rediscovered by %s\n", server_ip_address);
	     network_error = 1;
         }
      }
   }
  //printf("LEAVE hpsdrsim_discovery_thread\n");
}

void *
hpsdrsim_watchdog_thread (void *arg)
{
  int i, last_time = 0;
   struct stat sb;
   u_int last_sequence = 0xffffffff;

   //printf("ENTERING hpsdrsim_watchdog_thread active rcvrs: %d\n", mcb.active_num_rcvrs);

   // grab the last modification time on the config file if we're using it
   if (conf_file[0] != 0) {
      if (stat (conf_file, &sb) == -1) {
	 perror ("stat 1");
	 goto OUT_ERR;
      }
      last_time = sb.st_mtime;
   }


   //while(!running)
   sleep (2);

   // sleep for 1 second, check if we're sending packets
   while (1) {
      sleep (1);

      if (network_error) {
	 printf ("WARNING: A Network error occured, restarting...\n");
	 network_error = 0;
	 break;
      }

      if (last_sequence == hpsdr_sequence) {
	 if (do_exit)
	    exit (0);
	 printf ("WARNING: No hpsdr packets sent for 1 second, restarting...\n");
	 break;
      }

      last_sequence = hpsdr_sequence;

      // check to see if the config file changed
      if (conf_file[0] != 0) {
	 if (stat (conf_file, &sb) == -1) {
	    perror ("stat 2");
	    goto OUT_ERR;
	 }
	 if (last_time != sb.st_mtime) {
	    last_time = sb.st_mtime;
	    
	    // save our current per receiver config settings
	    update_config();

            if (parse_config (conf_file) == 0) {
              if (update_dongle () != 0) {
                printf ("ERROR: RTL Device Update Failed\n");
              }
            }

	    reset_cal = 1;
            /*
	    printf ("New settings:\n");
	    printf ("  number of rcvrs:\t%d\n", mcb.total_num_rcvrs);
	    printf ("  number of active rcvrs:\t%d\n", mcb.active_num_rcvrs);
	    printf ("  signal multiplier\t%d\n", mcb.signal_multiplier);
	    if (mcb.calibrate)
	       printf ("  calibration freq:\t%d hz\n", mcb.calibrate);
	    if (mcb.up_xtal)
	       printf ("  up_xtal freq:\t\t%d hz\n", mcb.up_xtal);
	    printf ("\nfreq_offset ");
            */
	    //	    for (i = 0; i < mcb.active_num_rcvrs; i++) {
              //  printf ("%d%s", mcb.freq_offset[i],
              //  (mcb.active_num_rcvrs - 1 != i) ? "," : "\n\n");
              //this should trigger a retune on each active dongle
	    //              mcb.rcb[i].new_freq = mcb.rcb[i].curr_freq;
	    // }
	 }
      }
   }

 OUT_ERR:
   running = revealed = 0;

   pthread_cancel (hpsdrsim_thread_id);
   pthread_cancel (discovery_thread_id);

   // unblock held mutexes so we can restart
   pthread_mutex_lock (&send_lock);
   send_flags = mcb.rcvrs_mask;
   pthread_cond_broadcast (&send_cond);
   pthread_mutex_unlock (&send_lock);

   if (!using_IQfiles) {
      pthread_mutex_lock (&iqready_lock);
      rcvr_flags = mcb.rcvrs_mask;
      pthread_cond_broadcast (&iqready_cond);
      pthread_mutex_unlock (&iqready_lock);
   }

   // set everything back to a 1 rcvr state
   mcb.rcvrs_mask = 1;
   mcb.rcb[0].rcvr_mask = 1;

   for (i = 1; i < mcb.total_num_rcvrs; i++) {
      mcb.rcb[i].rcvr_mask = 0;
      mcb.rcb[i].output_rate = 0;
   }

   mcb.active_num_rcvrs = 1;
   num_copy_rcvrs = 0;

   for (i = 0; i < MAX_RCVRS; i++)
      copy_rcvr[i] = -1;

   mcb.nsamps_packet = nsamps_packet[0];
   mcb.frame_offset1 = frame_offset1[0];
   mcb.frame_offset2 = frame_offset2[0];

   rcvr_flags &= 1;
   send_flags &= 1;
   last_num_rcvrs = last_rate = 0;
   mcb.output_rate = 48000;
   printf ("INFO: Setting hpsdr output rate to %d hz\n", mcb.output_rate);
   hpsdr_sequence = 0;
   /*
   if (mcb.calibrate) {
      printf ("\nfreq_offset ");
      for (i = 0; i < mcb.total_num_rcvrs; i++)
	 printf ("%d%s", mcb.freq_offset[i],
		 (mcb.total_num_rcvrs - 1 != i) ? "," : "\n\n");
   }
   */

   //printf("EXITING hpsdrsim_watchdog_thread\n");
   pthread_exit (NULL);
}

void *
do_cal_thr_func (void *arg)
{
   struct rcvr_cb *rcb;
   int i, n, tsleep;
   float var = 3000.0f;	// freq in hz from the calibration freq we care about
   float magnitude, last, current;
   float bin = (float) RTL_SAMPLE_RATE / FFT_SIZE;
   static int min_offset = 0, max_offset = 3000;
   static u_int first_pass = 1, first_pass_mask = 0;
   static int flip_offset[2][MAX_RCVRS];
   static int flip[MAX_RCVRS] = { 0 }, no_signal = 0;

   //printf("ENTERING do_cal_thr_func()\n");

   while (!do_exit && mcb.calibrate) {
      // we may do this if we re-parse the config file
      if (reset_cal) {
	 reset_cal = 0;
	 do_cal_time = 5;	// TODO, this needs a mutex
	 first_pass = 1;
	 first_pass_mask = 0;
	 no_signal = 0;
	 min_offset = 0;
	 max_offset = 3000;
	 memset (flip_offset, 0, sizeof (flip_offset));
	 memset (flip, 0, sizeof (flip));
      }

      pthread_mutex_lock (&do_cal_lock);
      while (mcb.cal_state < 0) {
	 pthread_cond_wait (&do_cal_cond, &do_cal_lock);
      }
      pthread_mutex_unlock (&do_cal_lock);
      if (mcb.cal_state == CAL_STATE_EXIT)
	 goto EXIT_DO_CAL;

      rcb = &mcb.rcb[mcb.cal_state];

      //printf("do_cal_thr_func() STATE 1 rcvr_num: %d\n", rcb->rcvr_num);

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
      while (mcb.cal_state == CAL_STATE_1) {
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

      //printf("do_cal_thr_func() STATE 2 rcvr_num: %d\n", rcb->rcvr_num);

      for (n = 0; n < (RTL_READ_COUNT / 2) / FFT_SIZE; ++n) {
	 for (i = 0; i < FFT_SIZE; ++i) {
	    // Normalize the IQ data and put it as the complex FFT input
	    mcb.fftIn[i][0] =
	       ((float) fft_buf[i * 2 + n * FFT_SIZE] - 127.0f) * 0.008f;
	    mcb.fftIn[i][1] =
	       ((float) fft_buf[i * 2 + n * FFT_SIZE + 1] - 127.0f) * 0.008f;
	 }
	 fftw_execute (mcb.fftPlan);
	 for (i = 0; i < FFT_SIZE; ++i) {
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
	   ++i) {
	 current = mcb.fft_averaged[(i + FFT_SIZE / 2) % FFT_SIZE];
	 if (current > last) {
	    //printf("DEBUG c:%f l:%f n:%d i:%d\n", current, last, n, i);
	    last = current;
	    n = i;
	 }
      }
      current = (float) mcb.calibrate + ((n - (FFT_SIZE / 2)) * bin);
      i = (int) current - mcb.calibrate;
      //printf("rcvr:%d i:%d n:%d l:%f c:%f o:%d\n", rcb->rcvr_num+1, i, n, last, current, (int)(current - mcb.calibrate));
      cal_rcvr_mask ^= 1 << rcb->rcvr_num;

      no_signal = (((FFT_SIZE / 2) == n)
		   && (current == mcb.calibrate)) ? no_signal + 1 : 0;
      if (10 == no_signal) {
	 no_signal = 0;
	 printf ("WARNING:  *** Calibration signal on %d Hz is very weak,"
		 " consider changing it! ***\n", mcb.calibrate);
      }

      // if n == 0 we won't update mcb.freq_offset[rcb->rcvr_num]
      n = abs (mcb.freq_offset[rcb->rcvr_num] - i);

      // only update offset if it's been established and we're not changing it drastically
      // also avoid flipping back and forth to a previous state
      if (i && (n < max_offset) && (n > min_offset)
	  && (i != flip_offset[1][rcb->rcvr_num])
	  && (i != flip_offset[0][rcb->rcvr_num])) {
	 rtlsdr_set_center_freq (rcb->rtldev,
				 rcb->curr_freq + mcb.up_xtal + i);
	 //printf
         //  ("INFO: [%s] cal update, rcvr %d old offset %+5d new offset %+5d new freq %d\n",
         //  time_stamp (), rcb->rcvr_num + 1, mcb.freq_offset[rcb->rcvr_num],
         //  i, rcb->curr_freq + i);

	 if (2 == flip[rcb->rcvr_num]) {
	    flip_offset[0][rcb->rcvr_num] = flip_offset[1][rcb->rcvr_num];
	    flip_offset[1][rcb->rcvr_num] = i;
	 }
	 else {
	    flip_offset[flip[rcb->rcvr_num]][rcb->rcvr_num] = i;
	    flip[rcb->rcvr_num] += 1;
	 }

	 mcb.freq_offset[rcb->rcvr_num] = i;

	 // after first pass cut back on the allowable offset
	 if (first_pass) {
	    first_pass_mask |= 1 << rcb->rcvr_num;
	    if (first_pass_mask == mcb.rcvrs_mask) {
	       first_pass = 0;
	       min_offset = ((int) bin) - 1;
	       max_offset = 200;
	    }
	 }

      }
      else {
#if 0
	 printf
	    ("INFO: [%s] NO cal update, rcvr %d old offset %+5d new offset %+5d new freq %d flip0 %d\n",
	     time_stamp (), rcb->rcvr_num + 1, mcb.freq_offset[rcb->rcvr_num],
	     i, rcb->curr_freq + i, flip_offset[0][rcb->rcvr_num]);
#endif
      }

      mcb.cal_state = CAL_STATE_3;
      //printf("do_cal_thr_func() STATE 3 rcvr_num: %d\n", rcb->rcvr_num);
   }
 EXIT_DO_CAL:
   pthread_exit (NULL);
   //printf("EXITING do_cal_thr_func()\n");
}

void
rtlsdr_callback (unsigned char *buf, uint32_t len, void *ctx)
{
   int i;
   struct rcvr_cb *rcb = (struct rcvr_cb *) ctx;
   float dcI, dcQ;

   if (do_exit || !running) {
      return;
   }

   if (RTL_READ_COUNT != len) {
      perror ("rtlsdr_callback(): RTL_READ_COUNT != len!\n");
      return;
   }

#if 0
   ftime (&test_end_time);
   printf ("test time %ld ms\n",
	   ((test_end_time.time * 1000) + test_end_time.millitm) -
	   ((test_start_time.time * 1000) + test_start_time.millitm));
   ftime (&test_start_time);
#endif

   // periodically calibrate each dongle if enabled
   if (mcb.calibrate) {
      cal_count[rcb->rcvr_num] += 1;

      if ((cal_rcvr < 0) || (cal_rcvr == rcb->rcvr_num)) {

	 if (mcb.cal_state == CAL_STATE_0) {
	    if ((cal_rcvr_mask & (1 << rcb->rcvr_num)) &&
		(cal_count[rcb->rcvr_num] > (do_cal_time * 20))) {
	       cal_rcvr = rcb->rcvr_num;
	       pthread_mutex_lock (&do_cal_lock);
	       mcb.cal_state = rcb->rcvr_num;
	       pthread_cond_broadcast (&do_cal_cond);
	       pthread_mutex_unlock (&do_cal_lock);
	       //printf("rtlsdr_callback() STATE 1 cmask %x\n", cal_rcvr_mask);
	    }
	 }
	 else if (mcb.cal_state == CAL_STATE_1) {
	    pthread_mutex_lock (&iqready_lock);
	    rcvr_flags |= rcb->rcvr_mask;
	    pthread_cond_broadcast (&iqready_cond);
	    pthread_mutex_unlock (&iqready_lock);
	    //printf("rtlsdr_callback() STATE 2 cmask %x\n", cal_rcvr_mask);
//RRK just pass buf to save time? seems to work OK
	    //memcpy(fft_buf, buf, RTL_READ_COUNT);
	    fft_buf = buf;
	    pthread_mutex_lock (&do_cal_lock);
	    mcb.cal_state = CAL_STATE_2;
	    pthread_cond_broadcast (&do_cal_cond);
	    pthread_mutex_unlock (&do_cal_lock);
	    return;
	 }
	 else if (mcb.cal_state == CAL_STATE_3) {
	    //printf("rtlsdr_callback() STATE 3 cmask %x\n", cal_rcvr_mask);
	    mcb.cal_state = CAL_STATE_0;
	    cal_rcvr = -1;
	    // do_cal_thr_func() will clear this bit on successful cal
	    if (!(cal_rcvr_mask & (1 << rcb->rcvr_num)))
	       cal_count[rcb->rcvr_num] = 0;
	 }

	 if (cal_rcvr_mask == 0) {
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
   for (i = 0; i < RTL_READ_COUNT; i += 2) {
#if 1				// remove DC component
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

   for (i = 8; i < 16; i++)
      payload[i] = proto_header[i - 8];

   for (i = 520; i < 528; i++)
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


   //printf("ENTERING rtl_read_thr_func() rcvr %d\n", i+1);
#if 1
   if (using_IQfiles) {
       if ((fp = fopen(rcb->filename, "rb")) != NULL) {
           while (!do_exit) {
               j = k = 0;
               if (strstr(rcb->filename, ".wav") != NULL) {
                  fread(&header, sizeof(WavHeader), 1, fp);
                  wav = true;
               }
               clock_gettime(CLOCK_MONOTONIC, &timeS);
               while (running) {
                   if (do_zero) rcb->iq_buf[j] = rcb->iq_buf[j+1] = ((float)rand()/(float)(RAND_MAX)) * 65536.0f;
                   else if (wav) { //RRK TODO, set sample rate/size from header info
                      fread(&IQ, sizeof(IQ), 1, fp);
                      rcb->iq_buf[j] = (float)((IQ << 16) & 0xffff0000);
                      fread(&IQ, sizeof(IQ), 1, fp);
                      r = (int)IQ;
                      rcb->iq_buf[j+1] = (float)((IQ << 16) & 0xffff0000);
                   } else {
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
	           if (k == 9600) {
                      pthread_mutex_lock (&iqready_lock);
                      rcvr_flags |= rcb->rcvr_mask;
                      pthread_cond_broadcast (&iqready_cond);
                      pthread_mutex_unlock (&iqready_lock);
                     break;
                   }
                   if ((message[0] == 0) && feof(fp)) {
                      if (using_IQrewind) {
                         rewind(fp);
                         strcpy(message, "Rewinding");
                      } else {
                         strcpy(message, "Ending");
                      }
                      time (&raw_time);
                      ptr_ts = gmtime (&raw_time);

                      // format time to SkimSrv's i.e. 2018-02-01 16:52:05Z
                      printf("rcvr %d, %s file %s at %4d-%02d-%02d %2d:%02d:%02dZ\n",
                          i+1, message, rcb->filename, ptr_ts->tm_year+1900, ptr_ts->tm_mon,
                          ptr_ts->tm_mday, ptr_ts->tm_hour, ptr_ts->tm_min, ptr_ts->tm_sec);

                      if (!strcmp("Ending", message)) {
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
               if(timeE.tv_nsec >= 1000000000) {
                   timeE.tv_nsec -= 1000000000;
                   timeE.tv_sec++;
               }
               clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &timeE, NULL);
           }
           fclose(fp);
       }
   } else {
       r = rtlsdr_read_async (rcb->rtldev, rtlsdr_callback,
			  (void *) (&mcb.rcb[i]), 2, RTL_READ_COUNT);
   }
#else // simulate an rtl read
   while (!do_exit || running) {
      usleep (5000);		// approximate
      pthread_mutex_lock (&iqready_lock);
      rcvr_flags |= rcb->rcvr_mask;
      pthread_cond_broadcast (&iqready_cond);
      pthread_mutex_unlock (&iqready_lock);
   }
   static u_char buf[RTL_READ_COUNT];

   while (!do_exit || running) {
      r = rtlsdr_read_sync (rcb->rtldev, buf, RTL_READ_COUNT, &i);
      if (RTL_READ_COUNT != i) {
	 perror ("rtlsdr_callback(): RTL_READ_COUNT != i!\n");
      }

      pthread_mutex_lock (&iqready_lock);

      for (i = 0; i < RTL_READ_COUNT; i += 2) {
	 rcb->iq_buf[i + 1] = rtl_lut[buf[i]];
	 rcb->iq_buf[i] = rtl_lut[buf[i + 1]];
      }
      rcvr_flags |= rcb->rcvr_mask;
      pthread_cond_broadcast (&iqready_cond);
      pthread_mutex_unlock (&iqready_lock);
   }
#endif
   //printf("EXITING rtl_read_thr_func() rcvr %d\n", i+1);
   pthread_exit (NULL);
}

int
init_rtl (int rcvr_num, int dev_index)
{
   int r;
   char num[16];
   rtlsdr_dev_t *rtldev;

   r = rtlsdr_open (&(mcb.rcb[rcvr_num].rtldev), dev_index);

   if (r < 0) {
      printf ("ERROR:  Failed to open rtlsdr device\n");
      return (-1);
   }

   rtldev = mcb.rcb[rcvr_num].rtldev;

   if (mcb.if_bw[rcvr_num] == 0) {
     printf ("Setting tuner IF BW to %d Hz\n", RTL_BANDWIDTH);
     r = rtlsdr_set_tuner_bandwidth (rtldev, RTL_BANDWIDTH);
     if (r < 0) {
       printf ("WARNING: Failed to set IF BW to %d Hz!\n",RTL_BANDWIDTH);
       return (-1);
     }

     mcb.if_bw[rcvr_num] = RTL_BANDWIDTH;
   } 
   else {
     printf ("Setting tuner IF BW to %d Hz\n", mcb.if_bw[rcvr_num]);
     r = rtlsdr_set_tuner_bandwidth (rtldev, mcb.if_bw[rcvr_num]);
     if (r < 0) {
       printf ("WARNING: Failed to set IF BW to %d Hz!\n",mcb.if_bw[rcvr_num]);
       return (-1);
     }
   } 


   r = rtlsdr_set_sample_rate (rtldev, RTL_SAMPLE_RATE);

   if (r < 0) {
      printf ("WARNING: Failed to set sample rate to %d!\n", RTL_SAMPLE_RATE);
      return (-1);
   }

   sprintf (num, "%d", mcb.gain[rcvr_num]);

   if (mcb.gain_mode[rcvr_num] > 0) {

     // Enable Auto Gain Mode
     r = verbose_auto_gain (rtldev);
     if (r < 0) {
       printf ("WARNING: Failed to set tuner gain to auto!\n");
       return (-1);
     }
     else
       printf ("  tuner gain\t\tauto\n");
   }
   else {
     // Set Manual Gain Mode and Gain setting value
     r = verbose_gain_set (rtldev, mcb.gain[rcvr_num]);
     if (r < 0) {
       printf ("WARNING: Failed to set tuner manual gain!\n");
       return (-1);
     }
     else
       printf ("  tuner gain\t\t%0.2f dB\n", mcb.gain[rcvr_num]/10.0);
   }

   rtlsdr_set_center_freq (rtldev, 100000000);
   if (r < 0)
      printf ("WARNING: Failed to set tuner freq to 100000000hz!\n");

   r = rtlsdr_set_direct_sampling (rtldev, mcb.direct_mode[rcvr_num]);

   if (r < 0) {
      printf ("WARNING: Failed to set direct sampling!\n");
      return (-1);
   }
   else
      printf ("  direct sampling\t%d\n", mcb.direct_mode[rcvr_num]);

   r = rtlsdr_set_agc_mode (rtldev, mcb.agc_mode[rcvr_num]);

   if (r < 0) {
      printf ("WARNING: Failed to set automatic gain!\n");
      return (-1);
   }
   else
      printf ("  agc mode\t\t%s\n",
	      (mcb.agc_mode[rcvr_num]) ? "on" : "off");

   r = rtlsdr_set_bias_tee (rtldev, mcb.bias_t[rcvr_num]);

   if (r < 0) {
      printf ("WARNING: Failed to set bias T!\n");
      return (-1);
   }
   else
      printf ("  bias_t\t\t%s\n\n",
	      (mcb.bias_t[rcvr_num]) ? "on" : "off");

   r = rtlsdr_reset_buffer (rtldev);

   if (r < 0) {
      printf ("WARNING: Failed to reset buffers!\n");
      return (-1);
   }

   return (0);
}

void
usage (char *progname)
{
   printf
      ("\n%s, an HPSDR Hermes simulator for RTL2832 based DVB-T receivers",
       progname);
   printf ("\nSee rtl_hpsdr.conf for configuration option descriptions.\n"
	   "\nUsage:\n" "\tPer rcvr options (comma separated i.e. 1,0,1,1):\n"
	   "\t[-a internal agc of the rtl2832 0|1 (defaults 0 or off)]\n"
	   "\t[-b turn on bias_t (if supported by dongle) 0|1 (defaults 0 or off)]\n"
	   "\t[-d direct sampling mode 0|1|2|3 (defaults 0 or off, 1=I 2=Q 3=NOMOD)]\n"
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
	   "\t[-i ip address assigned to network interface]\n"
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
   while ((token != NULL) && (count < MAX_RCVRS)) {
      strcpy (&(params[count++][0]), token);
      token = strtok (NULL, s);
   }

   for (i = 0; i < MAX_RCVRS; i++) {
      if (i < count) {
	 option[i] = atoi (params[i]);
	 option[MAX_RCVRS] = option[i];	// save last
      }
      else
	 option[i] = option[MAX_RCVRS];	// set to last
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
   while ((token != NULL) && (count < MAX_RCVRS)) {
      strcpy (&(params[count++][0]), token);
      token = strtok (NULL, s);
   }

   for (i = 0; i < MAX_RCVRS; i++) {
      if (i < count) {
	option[i] = (int)(atof(params[i]) * 10); /* tenths of a dB */
	 option[MAX_RCVRS] = option[i];	// save last
      }
      else
	 option[i] = option[MAX_RCVRS];	// set to last
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
   while ((token != NULL) && (count < MAX_RCVRS)) {
      strcpy (&(params[count++][0]), token);
      token = strtok (NULL, s);
   }

   for (i = 0; i < MAX_RCVRS; i++) {
      if (i < count) {
	strcpy(mcb.rcb[i].filename, params[i]);
	if(access(mcb.rcb[i].filename, F_OK) == -1 ) {
	   printf("ERROR File: %s does not exist!\n", mcb.rcb[i].filename);
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

   if ((fp = fopen (conf_file, "r")) != NULL) {
     //      printf ("\nParsing config file %s\n\n", conf_file);

      while (fgets (confbuf, MAXSTR, fp) != NULL) {
	 line++;

	 if (strchr (confbuf, '#') != NULL)
	    continue;

	 option[0] = value[0] = '\0';
	 sscanf (confbuf, "%s %s", option, value);

	 if (!strcmp ("direct_mode", option)) {
	    count = set_option (mcb.direct_mode, value);
	 }
	 else if (!strcmp ("tuner_gain", option)) {
	    count = set_gain_option (mcb.gain, value);
	 }
	 else if (!strcmp ("freq_offset", option)) {
	    count = set_option (mcb.freq_offset, value);
	 }
	 else if (!strcmp ("agc_mode", option)) {
	    count = set_option (mcb.agc_mode, value);
	 }
	 else if (!strcmp ("bias_t", option)) {
	    count = set_option (mcb.bias_t, value);
	 }
	 else if (!strcmp ("rcvr_order", option)) {
	    count = set_option (mcb.rcvr_order, value);
	 }
	 else if (!strcmp ("sound_dev", option)) {
	    strcpy (mcb.sound_dev, value);
	 }
	 else if (!strcmp ("signal_multiplier", option)) {
	    mcb.signal_multiplier = atoi (value);
	 }
	 else if (!strcmp ("total_num_rcvrs", option)) {
	    mcb.total_num_rcvrs = atoi (value);
	 }
	 else if (!strcmp ("up_xtal", option)) {
	    mcb.up_xtal = atoi (value);
	 }
	 else if (!strcmp ("calibrate", option)) {
	    mcb.calibrate = atoi (value);
	 }
	 else if (!strcmp ("ip_addr", option)) {
	    strcpy (mcb.ip_addr, value);
	 }
	 else if (!strcmp ("rtl_mode", option)) {
	   mcb.rtl_mode = atoi (value);
	 }
	 else if (!strcmp ("if_bw", option)) {
	   count = set_option (mcb.if_bw, value);
	 }
	 else if (!strcmp ("gain_mode", option)) {
	    count = set_option (mcb.gain_mode, value);
	 }
      }

      return (0);
   }
   else {
      printf ("Cannot find %s\n", conf_file);
      exit (-1);
   }
}

int
main (int argc, char *argv[])
{
   int i, opt, r = 0;
   bool loop = true;
   char serialstr[MAXSTR];
   char *progname = basename (argv[0]);
   char vendor[256] = { 0 }, product[256] = {
   0}, serial[256] = {
   0};
#ifdef _WIN32
   WSADATA wsd;

   i = WSAStartup (MAKEWORD (2, 2), &wsd);
#else
   struct sigaction sigact;
#endif

   // set defaults
   mcb.sound_dev[0] = 0;
   conf_file[0] = 0;
   mcb.output_rate = 48000;
   strcpy (mcb.ip_addr, "192.168.1.1");
   mcb.serialstr[0] = 0;
   mcb.signal_multiplier = 50;
   mcb.cal_state = CAL_STATE_0;
   mcb.calibrate = 0;
   mcb.up_xtal = 0;
   mcb.rtl_mode = RTL_MODE_SKIMMER;

   ftime (&test_start_time);

   // Initialize per receiver config settings
   for (i = 0; i < MAX_RCVRS; i++) {
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
	  && ((opt = getopt (argc, argv, "C:c:a:b:d:e:f:g:hi:l:m:o:p:r:s:v:W:w:")) !=
	      -1)) {
      switch (opt) {
	case 'a':
	   r = set_option (mcb.agc_mode, optarg);
	   break;

	case 'W':
	   using_IQrewind = true;
	   printf("IQ files will be looped\n");

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
	   printf ("\nGNU %s Version %s Date Built %s %s\n", progname,
		   PRG_VERSION, __TIME__, __DATE__);
	   printf ("GIT Hash %s\n", GITVERSION);
	   printf ("Copyright (C) 2014 Free Software Foundation, Inc.\n"
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

   if (!using_IQfiles) {
   r = rtlsdr_get_device_count ();

   if (r) {
      printf ("Found %d RTL device(s)", r);

      if ((mcb.total_num_rcvrs > r) || !mcb.total_num_rcvrs)
	 mcb.total_num_rcvrs = r;

      if (mcb.total_num_rcvrs > MAX_RCVRS)
	 mcb.total_num_rcvrs = MAX_RCVRS;

      printf (", using %d.\n", mcb.total_num_rcvrs);
   }
   else {
      printf ("No RTL devices found, exiting.\n");
      exit (0);
   }

   printf ("RTL base sample rate: %d hz\n\n", RTL_SAMPLE_RATE);
   printf ("Global settings:\n");
   printf ("  config file:\t\t%s\n",
	   (0 == conf_file[0]) ? "none" : conf_file);
   printf ("  ip address:\t\t%s\n", mcb.ip_addr);
   if (0 != mcb.serialstr[0])
      printf ("  eeprom serial string:\t%s\n", mcb.serialstr);
   printf ("  number of rcvrs:\t%d\n", mcb.total_num_rcvrs);
   printf ("  hpsdr output rate:\t%d hz\n", mcb.output_rate);
   printf ("  signal multiplier\t%d\n", mcb.signal_multiplier);
   if (mcb.calibrate)
      printf ("  calibration freq:\t%d hz\n", mcb.calibrate);
   if (mcb.up_xtal)
      printf ("  up_xtal freq:\t\t%d hz\n", mcb.up_xtal);
   printf ("  sound device:\t\t%s\n",
	   (0 == mcb.sound_dev[0]) ? "none" : mcb.sound_dev);
   } else {
       printf("Using IQ files, no RTL hardware will be used\n");
   }

#ifndef _WIN32
   sigact.sa_handler = rtl_sighandler;
   sigemptyset (&sigact.sa_mask);
   sigact.sa_flags = 0;
   sigaction (SIGINT, &sigact, NULL);
   sigaction (SIGTERM, &sigact, NULL);
   sigaction (SIGQUIT, &sigact, NULL);
   sigaction (SIGPIPE, &sigact, NULL);
#else
   SetConsoleCtrlHandler ((PHANDLER_ROUTINE) rtl_sighandler, TRUE);
#endif

   format_payload ();

   // create a lookup table for float values
   for (r = 0; r < 4; r++) {
      for (i = 0; i < 256; i++) {
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

#ifndef _WIN32
   if (mcb.sound_dev[0])
      open_local_sound (mcb.sound_dev);
#endif

   // enable the 1st rcvr until we get the active count
   mcb.rcb[0].rcvr_mask = 1;
   mcb.active_num_rcvrs = 1;
   mcb.rcvrs_mask = 1;
   mcb.nsamps_packet = nsamps_packet[0];
   mcb.frame_offset1 = frame_offset1[0];
   mcb.frame_offset2 = frame_offset2[0];

   mcb.fftPlan = fftw_plan_dft_1d (FFT_SIZE, mcb.fftIn,
				   mcb.fftOut, FFTW_FORWARD, FFTW_ESTIMATE);

   for (i = 0; i < mcb.total_num_rcvrs; i++) {
      mcb.rcb[i].mcb = &mcb;
      mcb.rcb[i].new_freq = 0;
      mcb.rcb[i].output_rate = 0;
      printf ("\nRcvr %d (ordered as %d) settings...\n", i + 1,
	      mcb.rcvr_order[i]);
      if (mcb.rcb[i].filename[0] == '\0') {
          rtlsdr_get_device_usb_strings (i, vendor, product, serial);
          printf ("  S/N \t\t\t%s\n", serial);
          printf ("  freq offset\t\t%d hz\n", mcb.freq_offset[i]);

          if (0 != init_rtl (i, mcb.rcvr_order[i] - 1)) {
              printf ("ERROR: Failed init_rtl rcvr%d hardware!\n", i + 1);
    	      return (-1);
          }
      } else
          printf ("Rcvr %d is reading IQ data from: %s\n", i + 1, mcb.rcb[i].filename);

      mcb.rcb[i].rtl_read_thr = rtl_read_thr[i];
      mcb.rcb[i].hpsdrsim_sendiq_thr = hpsdrsim_sendiq_thr[i];
      mcb.rcb[i].rcvr_num = i;
      if (using_IQfiles) {
      r = float_malloc_align ((void **) &(mcb.rcb[i].iq_buf), 16,
			      96000 * 2 * sizeof (float));
			      //RRK mcb.output_rate * 2 * sizeof (float));
      } else {
      r = float_malloc_align ((void **) &(mcb.rcb[i].iq_buf), 16,
			      RTL_READ_COUNT * sizeof (float));
      }

      if (r != 0) {
	 printf ("failed to allocate iq_buf aligned memory: r=%d\n", r);
	 return (r);
      }

      if ((r = pthread_create (&rtl_read_thr[i], NULL, rtl_read_thr_func,
			       &mcb.rcb[i]))) {
	 printf ("error: pthread_create, r: %d\n", r);
	 return (r);
      }

      if ((r = pthread_create (&hpsdrsim_sendiq_thr[i], NULL,
			       hpsdrsim_sendiq_thr_func, &mcb.rcb[i]))) {
	 printf ("pthread_create failed on hpsdrsim_sendiq_thr: r=%d\n", r);
	 return (r);
      }
   }

   if (mcb.calibrate) {
      if ((r = pthread_create (&do_cal_thr, NULL, do_cal_thr_func, &mcb))) {
	 printf ("pthread_create failed on do_cal_thr_func: r=%d\n", r);
	 return (r);
      }
   }

   while (!do_exit) {

      // reveal myself
      hpsdrsim_reveal ();

      while (revealed) {
	 // wait for all rcvrs to have formatted data ready before sending
	 pthread_mutex_lock (&send_lock);
	 while (send_flags != mcb.rcvrs_mask) {
	    pthread_cond_wait (&send_cond, &send_lock);
	 }

#if 0				// dump the frame for analysis
	 if (iloop++ == 100) {
	    iloop = 0;
	    printf ("rcvrs_mask:%x send_flags:%d\n", mcb.rcvrs_mask,
		    send_flags);

	    for (i = 0; i < HPSDR_FRAME_LEN; i++) {
	       printf ("%4d:%2x ", i, payload[i]);

	       if (!((i + 1) % 8))
		  printf ("\n");
	    }
	 }

//                      exit(0);
#endif
	 pthread_mutex_lock (&done_send_lock);

	 payload[4] = (hpsdr_sequence >> 24) & 0xff;
	 payload[5] = (hpsdr_sequence >> 16) & 0xff;
	 payload[6] = (hpsdr_sequence >> 8) & 0xff;
	 payload[7] = hpsdr_sequence & 0xff;

	 if (network_error) {
	    sleep(1); // wait for watchdog restart
	 } else if (sendto (reveal_socket, payload, HPSDR_FRAME_LEN, 0,
		     (struct sockaddr *) &their_addr,
		     sizeof (their_addr)) < 0) {
	    if (running) {
	       perror ("sendto() reveal_socket, error!\n");
	       network_error = 1;
	    }
	 }

	 hpsdr_sequence += 1;

	 pthread_mutex_unlock (&send_lock);
	 send_flags = 0;
	 pthread_cond_broadcast (&done_send_cond);
	 pthread_mutex_unlock (&done_send_lock);
      }
   }

   // clean up
   for (i = 0; i < mcb.total_num_rcvrs; i++) {
      if (!using_IQfiles) rtlsdr_close (mcb.rcb[i].rtldev);
      float_free_align (mcb.rcb[i].iq_buf);
   }

   if (close (reveal_socket) != 0)
      printf ("sockfd closing failed!\n");

#ifndef _WIN32
   if (mcb.sound_dev[0])
      close_local_sound ();
#else
   WSACleanup ();
#endif

   printf ("Exiting Program.\n");

   return 0;
}
