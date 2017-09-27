/*
 *			GPAC - Multimedia Framework C SDK
 *
 *			Authors: Jean Le Feuvre
 *			Copyright (c) Telecom ParisTech 2000-2012
 *					All rights reserved
 *
 *  This file is part of GPAC / AAC reader module
 *
 *  GPAC is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  GPAC is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <gpac/filters.h>
#include <gpac/constants.h>

#ifdef GPAC_HAS_FAAD

#ifdef __GNUC__
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#endif

#if !defined(__GNUC__)
# if defined(_WIN32_WCE) || defined (WIN32)
#  pragma comment(lib, "libfaad")
# endif
#endif

#include <faad.h>
#include <gpac/constants.h>
#include <gpac/avparse.h>


typedef struct
{
	faacDecHandle codec;
	faacDecFrameInfo info;
	u32 sample_rate, timescale, num_samples;
	u8 num_channels;

	GF_FilterPid *ipid, *opid;
	u32 cfg_crc;

	Bool signal_mc;
	Bool is_sbr;

	u32 channel_mask;
	char ch_reorder[16];
	u64 last_cts;
} GF_FAADCtx;

static void faaddec_check_mc_config(GF_FAADCtx *ctx)
{
	u32 i, channel_mask = 0;
	for (i=0; i<ctx->num_channels; i++) {
		switch (ctx->info.channel_position[i]) {
		case FRONT_CHANNEL_CENTER:
			channel_mask |= GF_AUDIO_CH_FRONT_CENTER;
			break;
		case FRONT_CHANNEL_LEFT:
			channel_mask |= GF_AUDIO_CH_FRONT_LEFT;
			break;
		case FRONT_CHANNEL_RIGHT:
			channel_mask |= GF_AUDIO_CH_FRONT_RIGHT;
			break;
		case SIDE_CHANNEL_LEFT:
			channel_mask |= GF_AUDIO_CH_SIDE_LEFT;
			break;
		case SIDE_CHANNEL_RIGHT:
			channel_mask |= GF_AUDIO_CH_SIDE_RIGHT;
			break;
		case BACK_CHANNEL_LEFT:
			channel_mask |= GF_AUDIO_CH_BACK_LEFT;
			break;
		case BACK_CHANNEL_RIGHT:
			channel_mask |= GF_AUDIO_CH_BACK_RIGHT;
			break;
		case BACK_CHANNEL_CENTER:
			channel_mask |= GF_AUDIO_CH_BACK_CENTER;
			break;
		case LFE_CHANNEL:
			channel_mask |= GF_AUDIO_CH_LFE;
			break;
		default:
			break;
		}
	}
	if (ctx->channel_mask != channel_mask) {
		ctx->channel_mask = channel_mask;
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CHANNEL_LAYOUT, &PROP_UINT(channel_mask) );
	}
}

static GF_Err faaddec_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	const GF_PropertyValue *p;
	u32 i, channel_mask=0;
	GF_FAADCtx *ctx = gf_filter_get_udta(filter);
#ifndef GPAC_DISABLE_AV_PARSERS
	GF_Err e;
	GF_M4ADecSpecInfo a_cfg;
#endif

	if (is_remove) {
		if (ctx->opid) gf_filter_pid_remove(ctx->opid);
		ctx->opid = NULL;
		ctx->ipid = NULL;
		return GF_OK;
	}
	if (! gf_filter_pid_check_caps(pid))
		return GF_NOT_SUPPORTED;

	p = gf_filter_pid_get_property(pid, GF_PROP_PID_DECODER_CONFIG);
	if (p && p->value.data && p->data_len) {
		u32 ex_crc = gf_crc_32(p->value.data, p->data_len);
		if (ctx->cfg_crc && (ctx->cfg_crc != ex_crc)) {
			//shoud we flush ?
			if (ctx->codec) faacDecClose(ctx->codec);
			ctx->codec = NULL;
		}
	} else {
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[FAAD] Reconfiguring withou DSI not yet supported\n"));
		return GF_NOT_SUPPORTED;
	}

	ctx->ipid = pid;

	ctx->codec = faacDecOpen();
	if (!ctx->codec) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[FAAD] Error initializing decoder\n"));
		return GF_IO_ERR;
	}

#ifndef GPAC_DISABLE_AV_PARSERS
	e = gf_m4a_get_config(p->value.data, p->data_len, &a_cfg);
	if (e) return e;
#endif
	if (faacDecInit2(ctx->codec, (unsigned char *)p->value.data, p->data_len, (unsigned long*)&ctx->sample_rate, (u8*)&ctx->num_channels) < 0)
	{
#ifndef GPAC_DISABLE_AV_PARSERS
		s8 res;
		char *dsi, *s_base_object_type;
		u32 dsi_len;
		switch (a_cfg.base_object_type) {
		case GF_M4A_AAC_MAIN:
			s_base_object_type = gf_stringizer(GF_M4A_AAC_MAIN);
			goto base_object_type_error;
		case GF_M4A_AAC_LC:
			s_base_object_type = gf_stringizer(GF_M4A_AAC_LC);
			goto base_object_type_error;
		case GF_M4A_AAC_SSR:
			s_base_object_type = gf_stringizer(GF_M4A_AAC_SSR);
			goto base_object_type_error;
		case GF_M4A_AAC_LTP:
			s_base_object_type = gf_stringizer(GF_M4A_AAC_LTP);
			goto base_object_type_error;
		case GF_M4A_AAC_SBR:
			s_base_object_type = gf_stringizer(GF_M4A_AAC_SBR);
			goto base_object_type_error;
		case GF_M4A_AAC_PS:
			s_base_object_type = gf_stringizer(GF_M4A_AAC_PS);
base_object_type_error: /*error case*/
			GF_LOG(GF_LOG_WARNING, GF_LOG_CODEC, ("[FAAD] Error: unsupported %s format for stream - defaulting to AAC LC\n", s_base_object_type));
		default:
			break;
		}
		a_cfg.base_object_type = GF_M4A_AAC_LC;
		a_cfg.has_sbr = GF_FALSE;
		a_cfg.nb_chan = a_cfg.nb_chan > 2 ? 1 : a_cfg.nb_chan;

		gf_m4a_write_config(&a_cfg, &dsi, &dsi_len);
		res = faacDecInit2(ctx->codec, (unsigned char *) dsi, dsi_len, (unsigned long *) &ctx->sample_rate, (u8 *) &ctx->num_channels);
		gf_free(dsi);
		if (res < 0)
#endif
		{
			GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[FAAD] Error when initializing AAC decoder for stream\n"));
			return GF_NOT_SUPPORTED;
		}
	}

#ifndef GPAC_DISABLE_AV_PARSERS
	ctx->is_sbr = a_cfg.has_sbr;
#endif
	ctx->num_samples = 1024;
	ctx->signal_mc = ctx->num_channels>2 ? GF_TRUE : GF_FALSE;

	if (!ctx->opid) {
		ctx->opid = gf_filter_pid_new(filter);
		gf_filter_pid_copy_properties(ctx->opid, ctx->ipid);
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_OTI, &PROP_UINT(GPAC_OTI_RAW_MEDIA_STREAM) );
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_AUDIO_FORMAT, &PROP_UINT(GF_AUDIO_FMT_S16) );
	}
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_SAMPLE_RATE, &PROP_UINT(ctx->sample_rate) );
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_SAMPLES_PER_FRAME, &PROP_UINT(ctx->num_samples) );
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_NUM_CHANNELS, &PROP_UINT(ctx->num_channels) );

	if (ctx->is_sbr) gf_filter_set_name(filter, "dec_faad:FAAD2 " FAAD2_VERSION " SBR mode");
	else gf_filter_set_name(filter,  "dec_faad:FAAD2 " FAAD2_VERSION);

	gf_filter_pid_set_framing_mode(pid, GF_TRUE);
	return GF_OK;
}

static s8 faaddec_get_channel_pos(GF_FAADCtx *ffd, u32 ch_cfg)
{
	u32 i;
	for (i=0; i<ffd->info.channels; i++) {
		switch (ffd->info.channel_position[i]) {
		case FRONT_CHANNEL_CENTER:
			if (ch_cfg==GF_AUDIO_CH_FRONT_CENTER) return i;
			break;
		case FRONT_CHANNEL_LEFT:
			if (ch_cfg==GF_AUDIO_CH_FRONT_LEFT) return i;
			break;
		case FRONT_CHANNEL_RIGHT:
			if (ch_cfg==GF_AUDIO_CH_FRONT_RIGHT) return i;
			break;
		case SIDE_CHANNEL_LEFT:
			if (ch_cfg==GF_AUDIO_CH_SIDE_LEFT) return i;
			break;
		case SIDE_CHANNEL_RIGHT:
			if (ch_cfg==GF_AUDIO_CH_SIDE_RIGHT) return i;
			break;
		case BACK_CHANNEL_LEFT:
			if (ch_cfg==GF_AUDIO_CH_BACK_LEFT) return i;
			break;
		case BACK_CHANNEL_RIGHT:
			if (ch_cfg==GF_AUDIO_CH_BACK_RIGHT) return i;
			break;
		case BACK_CHANNEL_CENTER:
			if (ch_cfg==GF_AUDIO_CH_BACK_CENTER) return i;
			break;
		case LFE_CHANNEL:
			if (ch_cfg==GF_AUDIO_CH_LFE) return i;
			break;
		}
	}
	return -1;
}

static GF_Err faaddec_process(GF_Filter *filter)
{
	GF_FAADCtx *ctx = gf_filter_get_udta(filter);
	void *buffer;
	char *output;
	unsigned short *conv_in, *conv_out;
	u32 i, j;
	Bool is_eos = GF_FALSE;
	GF_FilterPacket *dst_pck;
	GF_FilterPacket *pck = gf_filter_pid_get_packet(ctx->ipid);

	if (!pck) {
		is_eos = gf_filter_pid_is_eos(ctx->ipid);
		if (!is_eos) return GF_OK;
	}

	GF_LOG(GF_LOG_DEBUG, GF_LOG_CODEC, ("[FAAD] Decoding AU\n"));
	if (!pck) {
		buffer = faacDecDecode(ctx->codec, &ctx->info, NULL, 0);
	} else {
		Bool start, end;
		u32 size;
		const char *data = gf_filter_pck_get_data(pck, &size);
		buffer = faacDecDecode(ctx->codec, &ctx->info, data, size);

		gf_filter_pck_get_framing(pck, &start, &end);
		assert(start && end);
	}

	if (ctx->info.error>0) {
		GF_LOG(GF_LOG_WARNING, GF_LOG_CODEC, ("[FAAD] Error decoding AU %s\n", faacDecGetErrorMessage(ctx->info.error) ));
		if (pck) gf_filter_pid_drop_packet(ctx->ipid);
		return GF_NON_COMPLIANT_BITSTREAM;
	}
	if (!ctx->info.samples || !buffer || !ctx->info.bytesconsumed) {
		GF_LOG(GF_LOG_DEBUG, GF_LOG_CODEC, ("[FAAD] empty/non complete AU\n"));
		if (is_eos) gf_filter_pid_set_eos(ctx->opid);
		if (pck) gf_filter_pid_drop_packet(ctx->ipid);
		return GF_OK;
	}
	GF_LOG(GF_LOG_DEBUG, GF_LOG_CODEC, ("[FAAD] AU decoded\n"));

	/*FAAD froces us to decode a frame to get channel cfg*/
	if (ctx->signal_mc) {
		s32 ch, idx;
		ctx->signal_mc = GF_FALSE;
		idx = 0;
		/*NOW WATCH OUT!! FAAD may very well decide to output more channels than indicated!!!*/
		ctx->num_channels = ctx->info.channels;

		/*get cfg*/
		ch = faaddec_get_channel_pos(ctx, GF_AUDIO_CH_FRONT_LEFT);
		if (ch>=0) {
			ctx->ch_reorder[idx] = ch;
			idx++;
		}
		ch = faaddec_get_channel_pos(ctx, GF_AUDIO_CH_FRONT_RIGHT);
		if (ch>=0) {
			ctx->ch_reorder[idx] = ch;
			idx++;
		}
		ch = faaddec_get_channel_pos(ctx, GF_AUDIO_CH_FRONT_CENTER);
		if (ch>=0) {
			ctx->ch_reorder[idx] = ch;
			idx++;
		}
		ch = faaddec_get_channel_pos(ctx, GF_AUDIO_CH_LFE);
		if (ch>=0) {
			ctx->ch_reorder[idx] = ch;
			idx++;
		}
		ch = faaddec_get_channel_pos(ctx, GF_AUDIO_CH_BACK_LEFT);
		if (ch>=0) {
			ctx->ch_reorder[idx] = ch;
			idx++;
		}
		ch = faaddec_get_channel_pos(ctx, GF_AUDIO_CH_BACK_RIGHT);
		if (ch>=0) {
			ctx->ch_reorder[idx] = ch;
			idx++;
		}
		ch = faaddec_get_channel_pos(ctx, GF_AUDIO_CH_BACK_CENTER);
		if (ch>=0) {
			ctx->ch_reorder[idx] = ch;
			idx++;
		}
		ch = faaddec_get_channel_pos(ctx, GF_AUDIO_CH_SIDE_LEFT);
		if (ch>=0) {
			ctx->ch_reorder[idx] = ch;
			idx++;
		}
		ch = faaddec_get_channel_pos(ctx, GF_AUDIO_CH_SIDE_RIGHT);
		if (ch>=0) {
			ctx->ch_reorder[idx] = ch;
			idx++;
		}
		faaddec_check_mc_config(ctx);
	}

	dst_pck = gf_filter_pck_new_alloc(ctx->opid, sizeof(short) * ctx->info.samples, &output);
	if (!dst_pck) {
		if (pck) gf_filter_pid_drop_packet(ctx->ipid);
		return GF_OUT_OF_MEM;
	}
	if (pck) {
		ctx->last_cts = gf_filter_pck_get_cts(pck);
		ctx->timescale = gf_filter_pck_get_timescale(pck);
		gf_filter_pck_set_sap(dst_pck, gf_filter_pck_get_sap(pck));
	}
	gf_filter_pck_set_cts(dst_pck, ctx->last_cts);
	if (ctx->timescale != ctx->sample_rate) {
		u64 dur =ctx->info.samples * ctx->timescale;
		dur /= ctx->sample_rate;
		gf_filter_pck_set_duration(dst_pck, dur);
		ctx->last_cts += dur;
	} else {
		gf_filter_pck_set_duration(dst_pck, ctx->info.samples);
		ctx->last_cts += ctx->info.samples;
	}
	/*we assume left/right order*/
	if (ctx->num_channels<=2) {
		memcpy(output, buffer, sizeof(short)* ctx->info.samples);
		gf_filter_pck_send(dst_pck);
		if (pck) gf_filter_pid_drop_packet(ctx->ipid);
		return GF_OK;
	}

	conv_in = (unsigned short *) buffer;
	conv_out = (unsigned short *) output;
	for (i=0; i<ctx->info.samples; i+=ctx->info.channels) {
		for (j=0; j<ctx->info.channels; j++) {
			conv_out[i + j] = conv_in[i + ctx->ch_reorder[j]];
		}
	}
	return GF_OK;
}

static const GF_FilterCapability FAADInputs[] =
{
	{.code=GF_PROP_PID_STREAM_TYPE, PROP_UINT(GF_STREAM_AUDIO), .start=GF_TRUE},
	{.code=GF_PROP_PID_OTI, PROP_UINT(GPAC_OTI_AUDIO_AAC_MPEG4)},
	{.code=GF_PROP_PID_STREAM_TYPE, PROP_UINT(GF_STREAM_AUDIO), .start=GF_TRUE},
	{.code=GF_PROP_PID_OTI, PROP_UINT(GPAC_OTI_AUDIO_AAC_MPEG2_MP)},
	{.code=GF_PROP_PID_STREAM_TYPE, PROP_UINT(GF_STREAM_AUDIO), .start=GF_TRUE},
	{.code=GF_PROP_PID_OTI, PROP_UINT(GPAC_OTI_AUDIO_AAC_MPEG2_LCP)},
	{.code=GF_PROP_PID_STREAM_TYPE, PROP_UINT(GF_STREAM_AUDIO), .start=GF_TRUE},
	{.code=GF_PROP_PID_OTI, PROP_UINT(GPAC_OTI_AUDIO_AAC_MPEG2_SSRP)},

	{}
};

static const GF_FilterCapability FAADOutputs[] =
{
	{.code= GF_PROP_PID_STREAM_TYPE, PROP_UINT(GF_STREAM_AUDIO)},
	{.code= GF_PROP_PID_OTI, PROP_UINT( GPAC_OTI_RAW_MEDIA_STREAM )},

	{.code= GF_PROP_PID_STREAM_TYPE, PROP_UINT(GF_STREAM_VISUAL), .start=GF_TRUE},
	{.code= GF_PROP_PID_OTI, PROP_UINT( GPAC_OTI_RAW_MEDIA_STREAM )},

	{}
};

GF_FilterRegister FAADRegister = {
	.name = "faad",
	.description = "FAAD decoder",
	.private_size = sizeof(GF_FAADCtx),
	.input_caps = FAADInputs,
	.output_caps = FAADOutputs,
	.configure_pid = faaddec_configure_pid,
	.process = faaddec_process,
};

#endif

const GF_FilterRegister *faad_register(GF_FilterSession *session)
{
#ifdef GPAC_HAS_FAAD
	return &FAADRegister;
#else
	return NULL;
#endif
}
