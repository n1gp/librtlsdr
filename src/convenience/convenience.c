/*
 * Copyright (C) 2014 by Kyle Keen <keenerd@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/* a collection of user friendly tools
 * todo: use strtol for more flexible int parsing
 * */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include <assert.h>

#ifndef _WIN32
#include <unistd.h>
#include <sys/time.h>
#else
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#define _USE_MATH_DEFINES
#endif

#include <math.h>

#include "rtl-sdr.h"

double atofs(char *s)
/* standard suffixes */
{
	char last;
	int len;
	double suff = 1.0;
	len = strlen(s);
	/* allow formatting spaces from .csv command file */
	while ( len > 1 && isspace(s[len-1]) )	--len;
	last = s[len-1];
	s[len-1] = '\0';
	switch (last) {
		case 'g':
		case 'G':
			suff *= 1e3;
			/* fall-through */
		case 'm':
		case 'M':
			suff *= 1e3;
			/* fall-through */
		case 'k':
		case 'K':
			suff *= 1e3;
			suff *= atof(s);
			s[len-1] = last;
			return suff;
	}
	s[len-1] = last;
	return atof(s);
}

double atoft(char *s)
/* time suffixes, returns seconds */
{
	char last;
	int len;
	double suff = 1.0;
	len = strlen(s);
	last = s[len-1];
	s[len-1] = '\0';
	switch (last) {
		case 'h':
		case 'H':
			suff *= 60;
			/* fall-through */
		case 'm':
		case 'M':
			suff *= 60;
			/* fall-through */
		case 's':
		case 'S':
			suff *= atof(s);
			s[len-1] = last;
			return suff;
	}
	s[len-1] = last;
	return atof(s);
}

double atofp(char *s)
/* percent suffixes */
{
	char last;
	int len;
	double suff = 1.0;
	len = strlen(s);
	last = s[len-1];
	s[len-1] = '\0';
	switch (last) {
		case '%':
			suff *= 0.01;
			suff *= atof(s);
			s[len-1] = last;
			return suff;
	}
	s[len-1] = last;
	return atof(s);
}

int nearest_gain(rtlsdr_dev_t *dev, int target_gain)
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

int verbose_set_frequency(rtlsdr_dev_t *dev, uint32_t frequency)
{
	int r;
	r = rtlsdr_set_center_freq(dev, frequency);
	if (r < 0) {
		fprintf(stderr, "WARNING: Failed to set center freq.\n");
	} else {
		fprintf(stderr, "Tuned to %u Hz.\n", frequency);
	}
	return r;
}

int verbose_set_sample_rate(rtlsdr_dev_t *dev, uint32_t samp_rate)
{
	int r;
	r = rtlsdr_set_sample_rate(dev, samp_rate);
	if (r < 0) {
		fprintf(stderr, "WARNING: Failed to set sample rate.\n");
	} else {
		fprintf(stderr, "Sampling at %u S/s.\n", samp_rate);
	}
	return r;
}

int verbose_direct_sampling(rtlsdr_dev_t *dev, int on)
{
	int r;
	r = rtlsdr_set_direct_sampling(dev, on);
	if (r != 0) {
		fprintf(stderr, "WARNING: Failed to set direct sampling mode.\n");
		return r;
	}
	if (on == 0) {
		fprintf(stderr, "Direct sampling mode disabled.\n");}
	if (on == 1) {
		fprintf(stderr, "Enabled direct sampling mode, input 1/I.\n");}
	if (on == 2) {
		fprintf(stderr, "Enabled direct sampling mode, input 2/Q.\n");}
	return r;
}

int verbose_offset_tuning(rtlsdr_dev_t *dev)
{
	int r;
	r = rtlsdr_set_offset_tuning(dev, 1);
	if (r != 0) {
		fprintf(stderr, "WARNING: Failed to set offset tuning.\n");
	} else {
		fprintf(stderr, "Offset tuning mode enabled.\n");
	}
	return r;
}

int verbose_auto_gain(rtlsdr_dev_t *dev)
{
	int r;
	r = rtlsdr_set_tuner_gain_mode(dev, 0);
	if (r != 0) {
		fprintf(stderr, "WARNING: Failed to set tuner gain.\n");
	} else {
		fprintf(stderr, "Tuner gain set to automatic.\n");
	}
	return r;
}

int verbose_gain_set(rtlsdr_dev_t *dev, int gain)
{
	int r;
	r = rtlsdr_set_tuner_gain_mode(dev, 1);
	if (r < 0) {
		fprintf(stderr, "WARNING: Failed to enable manual gain.\n");
		return r;
	}
	r = rtlsdr_set_tuner_gain(dev, gain);
	if (r != 0) {
		fprintf(stderr, "WARNING: Failed to set tuner gain.\n");
	} else {
		fprintf(stderr, "Tuner gain set to %0.2f dB.\n", gain/10.0);
	}
	return r;
}

int verbose_ppm_set(rtlsdr_dev_t *dev, int ppm_error)
{
	int r;
	if (ppm_error == 0) {
		return 0;}
	r = rtlsdr_set_freq_correction(dev, ppm_error);
	if (r < 0) {
		fprintf(stderr, "WARNING: Failed to set ppm error.\n");
	} else {
		fprintf(stderr, "Tuner error set to %i ppm.\n", ppm_error);
	}
	return r;
}

int verbose_reset_buffer(rtlsdr_dev_t *dev)
{
	int r;
	r = rtlsdr_reset_buffer(dev);
	if (r < 0) {
		fprintf(stderr, "WARNING: Failed to reset buffers.\n");}
	return r;
}

int verbose_device_search(char *s)
{
	int i, device_count, device, offset;
	char *s2;
	char vendor[256], product[256], serial[256];
	device_count = rtlsdr_get_device_count();
	if (!device_count) {
		fprintf(stderr, "No supported devices found.\n");
		return -1;
	}
	fprintf(stderr, "Found %d device(s):\n", device_count);
	for (i = 0; i < device_count; i++) {
		rtlsdr_get_device_usb_strings(i, vendor, product, serial);
		fprintf(stderr, "  %d:  %s, %s, SN: %s\n", i, vendor, product, serial);
	}
	fprintf(stderr, "\n");
	/* does string look like raw id number */
	device = (int)strtol(s, &s2, 0);
	if (s2[0] == '\0' && device >= 0 && device < device_count) {
		fprintf(stderr, "Using device %d: %s\n",
			device, rtlsdr_get_device_name((uint32_t)device));
		return device;
	}
	/* does string exact match a serial */
	for (i = 0; i < device_count; i++) {
		rtlsdr_get_device_usb_strings(i, vendor, product, serial);
		if (strcmp(s, serial) != 0) {
			continue;}
		device = i;
		fprintf(stderr, "Using device %d: %s\n",
			device, rtlsdr_get_device_name((uint32_t)device));
		return device;
	}
	/* does string prefix match a serial */
	for (i = 0; i < device_count; i++) {
		rtlsdr_get_device_usb_strings(i, vendor, product, serial);
		if (strncmp(s, serial, strlen(s)) != 0) {
			continue;}
		device = i;
		fprintf(stderr, "Using device %d: %s\n",
			device, rtlsdr_get_device_name((uint32_t)device));
		return device;
	}
	/* does string suffix match a serial */
	for (i = 0; i < device_count; i++) {
		rtlsdr_get_device_usb_strings(i, vendor, product, serial);
		offset = strlen(serial) - strlen(s);
		if (offset < 0) {
			continue;}
		if (strncmp(s, serial+offset, strlen(s)) != 0) {
			continue;}
		device = i;
		fprintf(stderr, "Using device %d: %s\n",
			device, rtlsdr_get_device_name((uint32_t)device));
		return device;
	}
	fprintf(stderr, "No matching devices found.\n");
	return -1;
}

static struct tm * str_to_tm( const char * str, struct tm * t, double * fraction ) {
	char b[16];
	int k, v;
	/* 0         1         2   */
	/* 01234567890123456789012 */
	/* 2019-09-15T01:53:20.234 - mostly ISO 8601 */

	*fraction = 0.0;
	t->tm_sec	= 0;
	t->tm_min	= 0;
	t->tm_hour	= 0;
	t->tm_mday	= 1;
	t->tm_mon	= 0;
	t->tm_year	= 0;
	t->tm_wday	= 0;
	t->tm_yday	= 0;
	t->tm_isdst	= -1;

	/* date */
	if ( (str[4] == '-' || str[4] == '/') && str[4] == str[7] ) {
		/* year */
		b[4] = 0;	for ( k = 0; k < 4; ++k )	b[k] = str[k];
		v = atoi(b);
		t->tm_year = v - 1900;
		/* month */
		b[2] = 0;	for ( k = 0; k < 2; ++k )	b[k] = str[5+k];
		v = atoi(b);
		if (v < 1 || v > 12)
			return NULL;
		t->tm_mon = v - 1;
		/* day */
		b[2] = 0;	for ( k = 0; k < 2; ++k )	b[k] = str[8+k];
		v = atoi(b);
		if (v < 1 || v > 31)
			return NULL;
		t->tm_mday = v;
	} else
		return NULL;

	if (str[10] == 0 )
		return t;

	/* time */
	if ( str[10] != 'T' && str[10] != ' ' && str[10] != '_' )
		return NULL;
	if ( (str[13] == ':' || str[13] == '/') && str[13] == str[16] ) {
		/* hour */
		b[2] = 0;	for ( k = 0; k < 2; ++k )	b[k] = str[11+k];
		v = atoi(b);
		if (v < 0 || v > 23)
			return NULL;
		t->tm_hour = v;
		/* minute */
		b[2] = 0;	for ( k = 0; k < 2; ++k )	b[k] = str[14+k];
		v = atoi(b);
		if (v < 0 || v > 59)
			return NULL;
		t->tm_min = v;
		/* second */
		b[2] = 0;	for ( k = 0; k < 2; ++k )	b[k] = str[17+k];
		v = atoi(b);
		if (v < 0 || v > 61)
			return NULL;
		t->tm_sec = v;
	} else
		return NULL;

	if (str[19] == 0 )
		return t;

	/* fraction */
	if ( str[19] == '.' || str[19] == ',' ) {
		for ( k = 0; k < 16; ++k )	b[k] = 0;
		strcpy(b, "0.");
		strncpy(&b[2], &str[20], 12);
		*fraction = atof(b);
		return t;
	}

	/* return t anyway .. without fraction */
	return t;
}


time_t utctimestr_to_time(const char * str, double * fraction) {
	struct tm t;
	struct tm *p;
#ifdef _WIN32
	struct tm gtm;
	struct tm ltm;
	time_t nt;
	time_t gt;
	time_t lt;
#endif
	p = str_to_tm( str, &t, fraction );
	if (!p)
		return 0;
	p->tm_isdst = 0;
#ifndef _WIN32
	return timegm(p);
#else
	#ifdef _MSC_VER
		return _mkgmtime(p);
	#else
		/* workaround missing mkgmtime on mingw */
		nt = mktime(p);
		gtm = *gmtime(&nt);
		ltm = *localtime(&nt);
		gt = mktime(&gtm);
		lt = mktime(&ltm);
		assert( nt == gt );
		nt += ( lt - gt );
		return nt;
	#endif
#endif
}


time_t localtimestr_to_time(const char * str, double * fraction) {
	struct tm t;
	struct tm *p;

	p = str_to_tm( str, &t, fraction );
	
	if (!p)
		return 0;
#ifndef _WIN32
	return timelocal(p);
#else
	return mktime(p);
#endif
}



#ifndef _WIN32

void executeInBackground( char * file, char * args, char * searchStr[], char * replaceStr[] )
{
	pid_t pid;
	char * argv[256] = { NULL };
	int k, argc = 0;
	argv[argc++] = file;
	if (args) {
		argv[argc] = strtok(args, " ");
		while (argc < 256 && argv[argc]) {
			argv[++argc] = strtok(NULL, " ");
			for (k=0; argv[argc] && searchStr && replaceStr && searchStr[k] && replaceStr[k]; k++) {
				if (!strcmp(argv[argc], searchStr[k])) {
					argv[argc] = replaceStr[k];
					break;
				}
			}
		}
	}

	pid = fork();
	switch (pid)
	{
	case -1:
		/* Fork() has failed */
		fprintf(stderr, "error: fork for '%s' failed!\n", file);
		break;
	case 0:
		execvp(file, argv);
		fprintf(stderr, "error: execv of '%s' from within fork failed!\n", file);
		exit(10);
		break;
	default:
		/* This is processed by the parent */
		break;
	}
}

#else

void executeInBackground( char * file, char * args, char * searchStr[], char * replaceStr[] )
{
	char * argv[256] = { NULL };
	int k, argc = 0;
	argv[argc++] = file;
 	if (args) {
		argv[argc] = strtok(args, " \t");
		while (argc < 256 && argv[argc]) {
			argv[++argc] = strtok(NULL, " \t");
			for (k=0; argv[argc] && searchStr && replaceStr && searchStr[k] && replaceStr[k]; k++) {
				if (!strcmp(argv[argc], searchStr[k])) {
					argv[argc] = replaceStr[k];
					break;
				}
			}
		}
	}

	spawnvp(P_NOWAIT, file, argv);
}

#endif

// vim: tabstop=8:softtabstop=8:shiftwidth=8:noexpandtab
