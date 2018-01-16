/*	This file downsample.c is part of rtl_hpsdr.
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

#ifdef INCLUDE_NEON
#define vector_type float32x4_t
#define vector_zero vmovq_n_f32(0)
#define vector_load(x) vld1q_f32(x)
#define vector_mac(x,y,z) vmlaq_f32((x), vld1q_f32((y)), (z))
#define vector_store(x,y) vst1q_f32((x), (y))
#elif defined INCLUDE_SSE2
#define vector_type __m128
#define vector_zero _mm_setzero_ps()
#define vector_load(x) _mm_load_ps(x)
#define vector_mac(x,y,z) _mm_add_ps((x), _mm_mul_ps(_mm_load_ps((y)), (z)))
#define vector_store(x,y) _mm_store_ps((x), (y))
#else
#define vector_type float
#define vector_load(x) (*x)
#endif

#define MAX_LOOPS 5
#define FILTER_LEN 8

static float coeff_hb[FILTER_LEN] = {
	// 0.0, skip this as we want a multiple of 4
	0.240196626329132970,
	0.519606747341734180,
	0.240196626329132970,
	0.0,
};

// optimized for intrinsics
static float coeff_hb3[FILTER_LEN*2] = {
	0.240196626329132970,
	0.240196626329132970,
	0.519606747341734180,
	0.519606747341734180,
	0.240196626329132970,
	0.240196626329132970,
	0.0,
	0.0,
};

static float coeff_hb2[FILTER_LEN*2] = {
	 0.007459452182686200,
	 0.007459452182686200,
	-0.053643924388308561,
	-0.053643924388308561,
	 0.296225648097030230,
	 0.296225648097030230,
	 0.499917648217184480,
	 0.499917648217184480,
	 0.296225648097030230,
	 0.296225648097030230,
	-0.053643924388308561,
	-0.053643924388308561,
	 0.007459452182686200,
	 0.007459452182686200,
	 0.000000000000000000,
	 0.000000000000000000,
};

struct iq_bufs {
	float *in;
	float *out;
};

void
downsample(struct rcvr_cb* rcb) {
	int i, j, k, l;
	int count = RTL_READ_COUNT;
	float* pSrc;
	struct main_cb* mcb = rcb->mcb;
	static int last = 0, loop = 0;
	static float iq_buf_768[MAX_RCVRS][RTL_READ_COUNT/2] __attribute__((aligned(16)));
	static float iq_buf_384[MAX_RCVRS][RTL_READ_COUNT/4] __attribute__((aligned(16)));
	static float iq_buf_192[MAX_RCVRS][RTL_READ_COUNT/8] __attribute__((aligned(16)));
	static float iq_buf_96[MAX_RCVRS][RTL_READ_COUNT/16] __attribute__((aligned(16)));
	static struct iq_bufs iq_buf_loop[MAX_RCVRS][MAX_LOOPS];
	static vector_type h0, h1, h2, h3, v, s;

	// just need to do once through for each rcvr
	if (rcb->output_rate != mcb->output_rate) {
		rcb->output_rate = mcb->output_rate;
		h0 = vector_load(coeff_hb2);
		h1 = vector_load(coeff_hb2 + 4);
		h2 = vector_load(coeff_hb2 + 8);
		h3 = vector_load(coeff_hb2 + 12);
		iq_buf_loop[rcb->rcvr_num][0].in = rcb->iq_buf;
		iq_buf_loop[rcb->rcvr_num][0].out = iq_buf_768[rcb->rcvr_num];
		iq_buf_loop[rcb->rcvr_num][1].in = iq_buf_768[rcb->rcvr_num];
		iq_buf_loop[rcb->rcvr_num][1].out = iq_buf_384[rcb->rcvr_num];
		iq_buf_loop[rcb->rcvr_num][2].in = iq_buf_384[rcb->rcvr_num];
		iq_buf_loop[rcb->rcvr_num][2].out = iq_buf_192[rcb->rcvr_num];
		iq_buf_loop[rcb->rcvr_num][3].in = iq_buf_192[rcb->rcvr_num];
		iq_buf_loop[rcb->rcvr_num][3].out = iq_buf_96[rcb->rcvr_num];
		iq_buf_loop[rcb->rcvr_num][4].in = iq_buf_96[rcb->rcvr_num];

		switch(mcb->output_rate) {
		case 48000:
			loop = 5;
			last = 4;
			break;

		case 96000:
			loop = 4;
			last = 3;
			break;
	
		case 192000:
			loop = 3;
			last = 2;
			break;

		case 384000:
			loop = 2;
			last = 1;
			break;
		}
	}

	iq_buf_loop[rcb->rcvr_num][last].out = &(rcb->iqSamples[rcb->iqSamples_remaining * 2]);

	for(l = 0; l < loop; l++) {
		for(j = 0, k = 0; j < count; j += 2) {
			if(0 == (j % 4)) {
			pSrc = &(iq_buf_loop[rcb->rcvr_num][l].in[j]);
#if defined(INCLUDE_NEON) || defined(INCLUDE_SSE2)
			s = vector_zero;
			s = vector_mac(s, pSrc, h0);
			s = vector_mac(s, pSrc+4, h1);
			s = vector_mac(s, pSrc+8, h2);
			s = vector_mac(s, pSrc+12, h3);
			vector_store(rcb->dest, s);

			iq_buf_loop[rcb->rcvr_num][l].out[k++] = rcb->dest[0] + rcb->dest[2];
			iq_buf_loop[rcb->rcvr_num][l].out[k++] = rcb->dest[1] + rcb->dest[3];
#else
			v = s = 0.0f;

			// unroll for slight performance boost
			v += *pSrc * coeff_hb[0];
			s += *(pSrc + 1) * coeff_hb[0];
			v += *(pSrc + 2) * coeff_hb[1];
			s += *(pSrc + 3) * coeff_hb[1];
			v += *(pSrc + 4) * coeff_hb[2];
			s += *(pSrc + 5) * coeff_hb[2];
			v += *(pSrc + 6) * coeff_hb[3];
			s += *(pSrc + 7) * coeff_hb[3];

			// each loop is a half band lowpass so always downsample by 2 (% 4)
			iq_buf_loop[rcb->rcvr_num][l].out[k++] = v;
			iq_buf_loop[rcb->rcvr_num][l].out[k++] = s;
#endif
			}
		}

		count >>= 1;
	}
}
