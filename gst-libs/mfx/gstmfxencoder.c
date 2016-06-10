#include <mfxplugin.h>
#include "gstmfxencoder.h"
#include "gstmfxencoder_priv.h"
#include "gstmfxfilter.h"
#include "gstmfxsurfacepool.h"
#include "gstmfxvideometa.h"
#include "gstmfxsurfaceproxy.h"
#include "gstmfxtask.h"

#define DEBUG 1
#include "gstmfxdebug.h"

#define DEFAULT_ENCODER_PRESET      GST_MFX_ENCODER_PRESET_MEDIUM
#define DEFAULT_QUANTIZER           21
#define DEFAULT_ASYNC_DEPTH         4

/* Helper function to create a new encoder property object */
static GstMfxEncoderPropData *
prop_new(gint id, GParamSpec * pspec)
{
	GstMfxEncoderPropData *prop;

	if (!id || !pspec)
		return NULL;

	prop = g_slice_new(GstMfxEncoderPropData);
	if (!prop)
		return NULL;

	prop->prop = id;
	prop->pspec = g_param_spec_ref_sink(pspec);
	return prop;
}

/* Helper function to release a property object and any memory held herein */
static void
prop_free(GstMfxEncoderPropData * prop)
{
	if (!prop)
		return;

	if (prop->pspec) {
		g_param_spec_unref(prop->pspec);
		prop->pspec = NULL;
	}
	g_slice_free(GstMfxEncoderPropData, prop);
}

/* Helper function to lookup the supplied property specification */
static GParamSpec *
prop_find_pspec(GstMfxEncoder * encoder, gint prop_id)
{
	GPtrArray *const props = encoder->properties;
	guint i;

	if (props) {
		for (i = 0; i < props->len; i++) {
			GstMfxEncoderPropInfo *const prop = g_ptr_array_index(props, i);
			if (prop->prop == prop_id)
				return prop->pspec;
		}
	}
	return NULL;
}

/* Create a new array of properties, or NULL on error */
GPtrArray *
gst_mfx_encoder_properties_append(GPtrArray * props, gint prop_id,
	GParamSpec * pspec)
{
	GstMfxEncoderPropData *prop;

	if (!props) {
		props = g_ptr_array_new_with_free_func((GDestroyNotify)prop_free);
		if (!props)
			return NULL;
	}

	prop = prop_new(prop_id, pspec);
	if (!prop)
		goto error_allocation_failed;
	g_ptr_array_add(props, prop);
	return props;

	/* ERRORS */
error_allocation_failed:
	{
		GST_ERROR("failed to allocate encoder property info structure");
		g_ptr_array_unref(props);
		return NULL;
	}
}

/* Generate the common set of encoder properties */
GPtrArray *
gst_mfx_encoder_properties_get_default(const GstMfxEncoderClass * klass)
{
	const GstMfxEncoderClassData *const cdata = klass->class_data;
	GPtrArray *props = NULL;

	g_assert(cdata->rate_control_get_type != NULL);

	/**
	* GstMfxEncoder:rate-control
	*
	* The desired rate control mode, expressed as a #GstMfxRateControl.
	*/
	GST_MFX_ENCODER_PROPERTIES_APPEND(props,
		GST_MFX_ENCODER_PROP_RATECONTROL,
		g_param_spec_enum("rate-control",
            "Rate control", "Rate control mode",
            cdata->rate_control_get_type(), cdata->default_rate_control,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

	/**
	* GstMfxEncoder:bitrate
	*
	* The desired bitrate, expressed in kbps.
	*/
	GST_MFX_ENCODER_PROPERTIES_APPEND(props,
		GST_MFX_ENCODER_PROP_BITRATE,
		g_param_spec_uint("bitrate",
            "Bitrate (kbps)",
            "The desired bitrate expressed in kbps (0: auto-calculate)",
            0, 100 * 1024, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

	/**
	* GstMfxEncoder:idr-interval
	*
	* IDR-frame interval in terms of I-frames.
	*/
	GST_MFX_ENCODER_PROPERTIES_APPEND(props,
		GST_MFX_ENCODER_PROP_IDR_INTERVAL,
            g_param_spec_uint("idr-interval",
            "IDR interval",
            "Distance (in I-frames) between IDR frames",
            0, G_MAXINT, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

	/**
	* GstMfxEncoder:gop-size
	*
	* Number of pictures within the current GOP
	*/
	GST_MFX_ENCODER_PROPERTIES_APPEND(props,
		GST_MFX_ENCODER_PROP_GOP_SIZE,
		g_param_spec_uint("gop-size",
            "GOP size",
            "Number of pictures within the current GOP",
            0, G_MAXUINT16, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

	/**
	* GstMfxEncoder:gop-dist
	*
	* Distance between I- or P- key frames
	*/
	GST_MFX_ENCODER_PROPERTIES_APPEND(props,
		GST_MFX_ENCODER_PROP_GOP_REFDIST,
		g_param_spec_int("gop-distance",
            "GOP reference distance",
            "Distance between I- or P- key frames (1 means no B-frames)",
            -1, 32, -1, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

	/**
	* GstMfxEncoder:num-refs
	*
	* Number of reference frames
	*/
	GST_MFX_ENCODER_PROPERTIES_APPEND(props,
		GST_MFX_ENCODER_PROP_NUM_REFS,
		g_param_spec_uint("num-refs",
            "Number of reference frames",
            "Number of reference frames",
            0, 16, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

	/**
	* GstMfxEncoder:num-slices
	*
	* Number of slices in each video frame
	*/
	GST_MFX_ENCODER_PROPERTIES_APPEND(props,
		GST_MFX_ENCODER_PROP_NUM_SLICES,
		g_param_spec_uint("num-slices",
            "Number of slices",
            "Number of slices in each video frame",
            0, G_MAXINT, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    /**
	* GstMfxEncoder:quantizer
	*
	* Constant quantizer or quality to apply
	*/
	GST_MFX_ENCODER_PROPERTIES_APPEND(props,
		GST_MFX_ENCODER_PROP_QUANTIZER,
		g_param_spec_uint("quantizer",
            "Constant quantizer",
            "Constant quantizer or quality to apply", 0, 51,
            DEFAULT_QUANTIZER, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

	/**
	* GstMfxEncoder:qpi-offset
	*
	* Quantization parameter offset for I-frames
	*/
	GST_MFX_ENCODER_PROPERTIES_APPEND(props,
		GST_MFX_ENCODER_PROP_QPI,
		g_param_spec_uint("qpi-offset",
            "Quantization parameter offset for I-frames",
            "Quantization parameter offset for I-frames", 0, 51,
            0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

	/**
	* GstMfxEncoder:qpp-offset
	*
	* Quantization parameter offset for P-frames
	*/
	GST_MFX_ENCODER_PROPERTIES_APPEND(props,
		GST_MFX_ENCODER_PROP_QPP,
		g_param_spec_uint("qpp-offset",
            "Quantization parameter offset for P-frames",
            "Quantization parameter offset for P-frames", 0, 51,
            2, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

	/**
	* GstMfxEncoder:qpb-offset
	*
	* Quantization parameter offset for B-frames
	*/
	GST_MFX_ENCODER_PROPERTIES_APPEND(props,
		GST_MFX_ENCODER_PROP_QPB,
		g_param_spec_uint("qpb-offset",
            "Quantization parameter offset for B-frames",
            "Quantization parameter offset for B-frames", 0, 51,
            4, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    /**
	* GstMfxEncoder:mbbrc
	*
	* MB level bitrate control
	*/
	GST_MFX_ENCODER_PROPERTIES_APPEND(props,
		GST_MFX_ENCODER_PROP_MBBRC,
		g_param_spec_enum("mbbrc",
            "MB level bitrate control",
            "MB level bitrate control",
            GST_MFX_TYPE_OPTION, GST_MFX_OPTION_ON,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    /**
	* GstMfxEncoder:extbrc
	*
	* Extended bitrate control (deprecated)
	*/
    GST_MFX_ENCODER_PROPERTIES_APPEND(props,
		GST_MFX_ENCODER_PROP_EXTBRC,
		g_param_spec_enum("extbrc",
            "Extended bitrate control",
            "Extended bitrate control (deprecated)",
            GST_MFX_TYPE_OPTION, GST_MFX_OPTION_OFF,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    /**
	* GstMfxEncoder:adaptive-i
	*
	* Adaptive I-frame placement
	*/
    GST_MFX_ENCODER_PROPERTIES_APPEND(props,
		GST_MFX_ENCODER_PROP_ADAPTIVE_I,
		g_param_spec_enum("adaptive-i",
            "Adaptive I-frame placement",
            "Adaptive I-frame placement",
            GST_MFX_TYPE_OPTION, GST_MFX_OPTION_OFF,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    /**
	* GstMfxEncoder:adaptive-b
	*
	* Adaptive B-frame placement
	*/
    GST_MFX_ENCODER_PROPERTIES_APPEND(props,
		GST_MFX_ENCODER_PROP_ADAPTIVE_B,
		g_param_spec_enum("adaptive-b",
            "Adaptive B-frame placement",
            "Adaptive B-frame placement",
            GST_MFX_TYPE_OPTION, GST_MFX_OPTION_OFF,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    /**
	* GstMfxEncoder:b-pyramid
	*
	* Strategy to choose between I/P/B-frames
	*/
    GST_MFX_ENCODER_PROPERTIES_APPEND(props,
		GST_MFX_ENCODER_PROP_B_PYRAMID,
		g_param_spec_enum("b-pyramid",
            "Pyramidal B-frames",
            "Strategy to choose between I/P/B-frames",
            GST_MFX_TYPE_OPTION, GST_MFX_OPTION_AUTO,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    /**
	* GstMfxEncoder:accuracy
	*
	* Accuracy of AVBR rate control
	*/
	GST_MFX_ENCODER_PROPERTIES_APPEND(props,
		GST_MFX_ENCODER_PROP_ACCURACY,
		g_param_spec_uint("accuracy",
            "AVBR accuracy",
            "Accuracy of AVBR rate control", 0, G_MAXUINT16,
            0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    /**
	* GstMfxEncoder:convergence
	*
	* Convergence of AVBR rate control
	*/
	GST_MFX_ENCODER_PROPERTIES_APPEND(props,
		GST_MFX_ENCODER_PROP_CONVERGENCE,
		g_param_spec_uint("convergence",
            "AVBR convergence",
            "Convergence of AVBR rate control", 0, G_MAXUINT16,
            0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

	/**
	* GstMfxEncoder:async-depth
	*
	* Number of parallel operations before explicit sync
	*/
	GST_MFX_ENCODER_PROPERTIES_APPEND(props,
		GST_MFX_ENCODER_PROP_ASYNC_DEPTH,
		g_param_spec_uint("async-depth",
		"Asynchronous depth",
		"Number of parallel operations before explicit sync", 0, 20,
		DEFAULT_ASYNC_DEPTH, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

	/**
	* GstMfxEncoder:preset
	*
	* The desired encoder preset option.
	*/
	GST_MFX_ENCODER_PROPERTIES_APPEND(props,
		GST_MFX_ENCODER_PROP_PRESET,
		g_param_spec_enum("preset",
		"Encoder Preset",
		"Encoder preset option",
		GST_MFX_TYPE_ENCODER_PRESET, DEFAULT_ENCODER_PRESET,
		G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

	return props;
}

static void
set_default_option_values(GstMfxEncoder * encoder)
{
    /* Extended coding options, introduced in API 1.0 */
    encoder->extco.MECostType           = 0; // reserved, must be 0
    encoder->extco.MESearchType         = 0; // reserved, must be 0
    encoder->extco.MVSearchWindow.x     = 0; // reserved, must be 0
    encoder->extco.MVSearchWindow.y     = 0; // reserved, must be 0
    encoder->extco.RefPicListReordering = 0; // reserved, must be 0
    encoder->extco.IntraPredBlockSize   = 0; // reserved, must be 0
    encoder->extco.InterPredBlockSize   = 0; // reserved, must be 0
    encoder->extco.MVPrecision          = 0; // reserved, must be 0
    encoder->extco.EndOfSequence        = MFX_CODINGOPTION_UNKNOWN;
    encoder->extco.RateDistortionOpt    = MFX_CODINGOPTION_UNKNOWN;
    encoder->extco.ResetRefList         = MFX_CODINGOPTION_UNKNOWN;
    encoder->extco.MaxDecFrameBuffering = 0; // unspecified
    encoder->extco.AUDelimiter          = MFX_CODINGOPTION_OFF;
    encoder->extco.SingleSeiNalUnit     = MFX_CODINGOPTION_UNKNOWN;
    encoder->extco.PicTimingSEI         = MFX_CODINGOPTION_OFF;
    encoder->extco.VuiNalHrdParameters  = MFX_CODINGOPTION_UNKNOWN;
    encoder->extco.FramePicture         = MFX_CODINGOPTION_UNKNOWN;
    encoder->extco.RefPicMarkRep        = MFX_CODINGOPTION_UNKNOWN;
    encoder->extco.FieldOutput          = MFX_CODINGOPTION_UNKNOWN;
    encoder->extco.NalHrdConformance    = MFX_CODINGOPTION_UNKNOWN;
    encoder->extco.SingleSeiNalUnit     = MFX_CODINGOPTION_UNKNOWN;
    encoder->extco.VuiVclHrdParameters  = MFX_CODINGOPTION_UNKNOWN;
    encoder->extco.ViewOutput           = MFX_CODINGOPTION_UNKNOWN;
    encoder->extco.RecoveryPointSEI     = MFX_CODINGOPTION_UNKNOWN;

    /* Extended coding options 2, introduced in API 1.6 */
    encoder->extco2.IntRefType      = 0;
    encoder->extco2.IntRefCycleSize = 2;
    encoder->extco2.IntRefQPDelta   = 0;
    encoder->extco2.MaxFrameSize    = 0;
    encoder->extco2.BitrateLimit    = MFX_CODINGOPTION_ON;
    encoder->extco2.MBBRC           = MFX_CODINGOPTION_UNKNOWN;
    encoder->extco2.ExtBRC          = MFX_CODINGOPTION_UNKNOWN;
    //encoder->extco2.LookAheadDepth  = 40;
    encoder->extco2.RepeatPPS       = MFX_CODINGOPTION_ON;
    encoder->extco2.BRefType        = MFX_B_REF_UNKNOWN;
    encoder->extco2.AdaptiveI       = MFX_CODINGOPTION_UNKNOWN;
    encoder->extco2.AdaptiveB       = MFX_CODINGOPTION_UNKNOWN;
    //encoder->extco2.LookAheadDS     = MFX_LOOKAHEAD_DS_OFF;
    encoder->extco2.NumMbPerSlice   = 0;
}

static void
set_extended_coding_options(GstMfxEncoder * encoder)
{
	encoder->extco.Header.BufferId = MFX_EXTBUFF_CODING_OPTION;
	encoder->extco.Header.BufferSz = sizeof(encoder->extco);

	encoder->extco2.Header.BufferId = MFX_EXTBUFF_CODING_OPTION2;
	encoder->extco2.Header.BufferSz = sizeof(encoder->extco2);

	set_default_option_values(encoder);

    if (encoder->mbbrc != GST_MFX_OPTION_AUTO)
        encoder->extco2.MBBRC = encoder->mbbrc ?
            MFX_CODINGOPTION_ON : MFX_CODINGOPTION_OFF;
    if (encoder->extbrc != GST_MFX_OPTION_AUTO)
        encoder->extco2.ExtBRC = encoder->extbrc ?
            MFX_CODINGOPTION_ON : MFX_CODINGOPTION_OFF;
    if (encoder->adaptive_i != GST_MFX_OPTION_AUTO)
        encoder->extco2.AdaptiveI = encoder->adaptive_i ?
            MFX_CODINGOPTION_ON : MFX_CODINGOPTION_OFF;
    if (encoder->adaptive_b != GST_MFX_OPTION_AUTO)
        encoder->extco2.AdaptiveB = encoder->adaptive_b ?
            MFX_CODINGOPTION_ON : MFX_CODINGOPTION_OFF;
    if (encoder->b_strategy != GST_MFX_OPTION_AUTO)
        encoder->extco2.BRefType = encoder->b_strategy ?
            MFX_B_REF_PYRAMID : MFX_B_REF_OFF;

    if (MFX_CODEC_AVC == encoder->codec) {
        if (encoder->max_slice_size >= 0)
            encoder->extco2.MaxSliceSize = encoder->max_slice_size;
        encoder->extco.CAVLC = !encoder->use_cabac ?
            MFX_CODINGOPTION_ON : MFX_CODINGOPTION_OFF;
        encoder->extco2.Trellis = encoder->trellis;
    }

    switch (encoder->rc_method) {
    case GST_MFX_RATECONTROL_LA_BRC:
    case GST_MFX_RATECONTROL_LA_ICQ:
    case GST_MFX_RATECONTROL_LA_HRD:
        if (!encoder->la_depth)
            encoder->la_depth = 40;
        encoder->extco2.LookAheadDepth = CLAMP(encoder->la_depth, 10, 100);
        encoder->extco2.LookAheadDS = encoder->look_ahead_downsampling;
        break;
    default:
        break;
    }

    encoder->extparam_internal[encoder->params.NumExtParam++] =
		(mfxExtBuffer *)&encoder->extco;
	encoder->extparam_internal[encoder->params.NumExtParam++] =
        (mfxExtBuffer *)&encoder->extco2;

    encoder->params.ExtParam = encoder->extparam_internal;
}

static void
set_encoding_params(GstMfxEncoder * encoder)
{
	switch (encoder->rc_method) {
    case GST_MFX_RATECONTROL_CQP:
        encoder->params.mfx.QPI =
            CLAMP(encoder->global_quality + encoder->qpi_offset, 0, 51);
        encoder->params.mfx.QPP =
            CLAMP(encoder->global_quality + encoder->qpp_offset, 0, 51);
        encoder->params.mfx.QPB =
            CLAMP(encoder->global_quality + encoder->qpb_offset, 0, 51);

        /* If set to auto, then enable b-pyramid */
        if (GST_MFX_OPTION_AUTO == encoder->b_strategy)
            encoder->b_strategy = GST_MFX_OPTION_ON;
        encoder->gop_size = 32;
        encoder->gop_refdist =
            encoder->gop_refdist < 0 ? 4 : encoder->gop_refdist;
        break;
    case GST_MFX_RATECONTROL_AVBR:
        encoder->params.mfx.Convergence = encoder->avbr_convergence;
        encoder->params.mfx.Accuracy = encoder->avbr_accuracy;
        break;
    case GST_MFX_RATECONTROL_ICQ:
    case GST_MFX_RATECONTROL_LA_ICQ:
        encoder->params.mfx.ICQQuality = CLAMP(encoder->global_quality, 1, 51);
        break;
    default:
        break;
    }

    encoder->params.AsyncDepth = encoder->async_depth;
	encoder->params.mfx.TargetUsage = encoder->preset;
	encoder->params.mfx.RateControlMethod = encoder->rc_method;
	encoder->params.mfx.IdrInterval = encoder->idr_interval;
	encoder->params.mfx.NumRefFrame = CLAMP(encoder->num_refs, 0, 16);
	encoder->params.mfx.GopPicSize = encoder->gop_size;
	encoder->params.mfx.NumSlice = encoder->num_slices;

	if (encoder->bitrate)
		encoder->params.mfx.TargetKbps = encoder->bitrate;
	if (encoder->profile == GST_MFX_PROFILE_AVC_CONSTRAINED_BASELINE ||
            encoder->profile == GST_MFX_PROFILE_AVC_BASELINE)
        encoder->params.mfx.GopRefDist = 1;
    else
        encoder->params.mfx.GopRefDist = CLAMP(
            encoder->gop_refdist < 0 ? 3 : encoder->gop_refdist, 0, 32);

    set_extended_coding_options(encoder);
}

static void
gst_mfx_encoder_set_input_params(GstMfxEncoder * encoder)
{
    encoder->params.mfx.CodecProfile =
        gst_mfx_profile_get_codec_profile(encoder->profile);
    if (encoder->level)
        encoder->params.mfx.CodecLevel = encoder->level;

	encoder->params.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
	encoder->params.mfx.FrameInfo.FourCC = MFX_FOURCC_NV12;
	encoder->params.mfx.FrameInfo.PicStruct =
        GST_VIDEO_INFO_IS_INTERLACED(&encoder->info) ?
		(GST_VIDEO_INFO_FLAG_IS_SET(&encoder->info, GST_VIDEO_FRAME_FLAG_TFF) ?
			MFX_PICSTRUCT_FIELD_TFF : MFX_PICSTRUCT_FIELD_BFF)
		: MFX_PICSTRUCT_PROGRESSIVE;

	encoder->params.mfx.FrameInfo.CropX = 0;
	encoder->params.mfx.FrameInfo.CropY = 0;
	encoder->params.mfx.FrameInfo.CropW = encoder->info.width;
	encoder->params.mfx.FrameInfo.CropH = encoder->info.height;
	encoder->params.mfx.FrameInfo.FrameRateExtN =
		encoder->info.fps_n ? encoder->info.fps_n : 30;
	encoder->params.mfx.FrameInfo.FrameRateExtD = encoder->info.fps_d;
	encoder->params.mfx.FrameInfo.AspectRatioW = encoder->info.par_n;
	encoder->params.mfx.FrameInfo.AspectRatioH = encoder->info.par_d;
	encoder->params.mfx.FrameInfo.BitDepthChroma = 8;
	encoder->params.mfx.FrameInfo.BitDepthLuma = 8;

	if (encoder->codec == MFX_CODEC_HEVC &&
			!g_strcmp0(encoder->plugin_uid, "6fadc791a0c2eb479ab6dcd5ea9da347")) {
		encoder->params.mfx.FrameInfo.Width = GST_ROUND_UP_32(encoder->info.width);
		encoder->params.mfx.FrameInfo.Height = GST_ROUND_UP_32(encoder->info.height);
	}
	else {
		encoder->params.mfx.FrameInfo.Width = GST_ROUND_UP_16(encoder->info.width);
		encoder->params.mfx.FrameInfo.Height =
			(MFX_PICSTRUCT_PROGRESSIVE == encoder->params.mfx.FrameInfo.PicStruct) ?
			GST_ROUND_UP_16(encoder->info.height) :
			GST_ROUND_UP_32(encoder->info.height);
	}

	set_encoding_params(encoder);
}

static gboolean
gst_mfx_encoder_init_properties(GstMfxEncoder * encoder,
	GstMfxTaskAggregator * aggregator, GstVideoInfo * info, gboolean mapped)
{
	encoder->aggregator = gst_mfx_task_aggregator_ref(aggregator);
	encoder->encode_task = gst_mfx_task_new(encoder->aggregator,
		GST_MFX_TASK_ENCODER);
	if (!encoder->encode_task)
		return FALSE;

	gst_mfx_task_aggregator_set_current_task(encoder->aggregator,
		encoder->encode_task);
	encoder->session = gst_mfx_task_get_session(encoder->encode_task);

	encoder->bitstream = g_byte_array_new();
	if (!encoder->bitstream)
		return FALSE;

	encoder->params.mfx.CodecId = encoder->codec;
	encoder->info = *info;
	encoder->mapped = mapped;

    /* Use input system memory with raw NV12 surfaces */
	if (mapped && GST_VIDEO_INFO_FORMAT(info) == GST_VIDEO_FORMAT_NV12) {
        encoder->params.IOPattern = MFX_IOPATTERN_IN_SYSTEM_MEMORY;
	}
	else {
        encoder->params.IOPattern = MFX_IOPATTERN_IN_VIDEO_MEMORY;
        gst_mfx_task_use_video_memory(encoder->encode_task);
	}

	return TRUE;
}

/* Base encoder initialization (internal) */
static gboolean
gst_mfx_encoder_init(GstMfxEncoder * encoder,
	GstMfxTaskAggregator * aggregator, GstVideoInfo * info, gboolean mapped)
{
	GstMfxEncoderClass *const klass = GST_MFX_ENCODER_GET_CLASS(encoder);

	g_return_val_if_fail(aggregator != NULL, FALSE);

#define CHECK_VTABLE_HOOK(FUNC) do {            \
	if (!klass->FUNC)                           \
	goto error_invalid_vtable;                \
	} while (0)

	CHECK_VTABLE_HOOK(init);
	CHECK_VTABLE_HOOK(finalize);
	CHECK_VTABLE_HOOK(get_default_properties);

#undef CHECK_VTABLE_HOOK

	if (!klass->init(encoder))
		return FALSE;
	if (!gst_mfx_encoder_init_properties(encoder, aggregator, info, mapped))
		return FALSE;
	return TRUE;

	/* ERRORS */
error_invalid_vtable:
	{
		GST_ERROR("invalid subclass hook (internal error)");
		return FALSE;
	}
}

/* Base encoder cleanup (internal) */
void
gst_mfx_encoder_finalize(GstMfxEncoder * encoder)
{
	GstMfxEncoderClass *const klass = GST_MFX_ENCODER_GET_CLASS(encoder);

	klass->finalize(encoder);

    gst_mfx_filter_replace(&encoder->filter, NULL);
	MFXVideoENCODE_Close(encoder->session);

	g_byte_array_unref(encoder->bitstream);
	gst_mfx_task_aggregator_unref(encoder->aggregator);
	gst_mfx_task_replace(&encoder->encode_task, NULL);

	if (encoder->properties) {
		g_ptr_array_unref(encoder->properties);
		encoder->properties = NULL;
	}
}

GstMfxEncoder *
gst_mfx_encoder_new(const GstMfxEncoderClass * klass,
	GstMfxTaskAggregator * aggregator, GstVideoInfo * info, gboolean mapped)
{
	GstMfxEncoder *encoder;

	g_return_val_if_fail(aggregator != NULL, NULL);

	encoder = gst_mfx_mini_object_new0(GST_MFX_MINI_OBJECT_CLASS(klass));
	if (!encoder)
		goto error;

	if (!gst_mfx_encoder_init(encoder, aggregator, info, mapped))
		goto error;

	return encoder;
error:
	gst_mfx_mini_object_unref(encoder);
	return NULL;
}

GstMfxEncoder *
gst_mfx_encoder_ref(GstMfxEncoder * encoder)
{
	g_return_val_if_fail(encoder != NULL, NULL);

	return gst_mfx_mini_object_ref(GST_MFX_MINI_OBJECT(encoder));
}

void
gst_mfx_encoder_unref(GstMfxEncoder * encoder)
{
	gst_mfx_mini_object_unref(GST_MFX_MINI_OBJECT(encoder));
}

void
gst_mfx_encoder_replace(GstMfxEncoder ** old_encoder_ptr,
	GstMfxEncoder * new_encoder)
{
	g_return_if_fail(old_encoder_ptr != NULL);

	gst_mfx_mini_object_replace((GstMfxMiniObject **)old_encoder_ptr,
		GST_MFX_MINI_OBJECT(new_encoder));
}

gboolean
gst_mfx_encoder_set_async_depth(GstMfxEncoder * encoder, mfxU16 async_depth)
{
    g_return_val_if_fail(async_depth <= 20, FALSE);

    encoder->async_depth = async_depth;
    return TRUE;
}

gboolean
gst_mfx_encoder_set_gop_refdist(GstMfxEncoder * encoder, gint gop_refdist)
{
    g_return_val_if_fail(gop_refdist <= 32, FALSE);

	encoder->gop_refdist = gop_refdist;
	return TRUE;
}

gboolean
gst_mfx_encoder_set_num_references(GstMfxEncoder * encoder, mfxU16 num_refs)
{
    g_return_val_if_fail(num_refs <= 16, FALSE);

	encoder->num_refs = num_refs;
	return TRUE;
}

gboolean
gst_mfx_encoder_set_quantizer(GstMfxEncoder * encoder, guint quantizer)
{
    g_return_val_if_fail(quantizer <= 51, FALSE);

	encoder->global_quality = quantizer;
	return TRUE;
}

gboolean
gst_mfx_encoder_set_qpi_offset(GstMfxEncoder * encoder, mfxU16 offset)
{
    g_return_val_if_fail(offset <= 51, FALSE);

	encoder->qpi_offset = offset;
	return TRUE;
}

gboolean
gst_mfx_encoder_set_qpp_offset(GstMfxEncoder * encoder, mfxU16 offset)
{
    g_return_val_if_fail(offset <= 51, FALSE);

	encoder->qpp_offset = offset;
	return TRUE;
}

gboolean
gst_mfx_encoder_set_qpb_offset(GstMfxEncoder * encoder, mfxU16 offset)
{
    g_return_val_if_fail(offset <= 51, FALSE);

	encoder->qpb_offset = offset;
	return TRUE;
}

GstMfxEncoderStatus
gst_mfx_encoder_start(GstMfxEncoder *encoder)
{
	mfxStatus sts = MFX_ERR_NONE;
	mfxFrameAllocRequest enc_request;
	mfxFrameAllocResponse enc_response;

	memset(&enc_request, 0, sizeof (mfxFrameAllocRequest));

	gst_mfx_encoder_set_input_params(encoder);

	sts = MFXVideoENCODE_Query(encoder->session, &encoder->params,
				&encoder->params);
	if (sts > 0)
		GST_WARNING("Incompatible video params detected %d", sts);

	sts = MFXVideoENCODE_QueryIOSurf(encoder->session, &encoder->params,
				&enc_request);
	if (sts < 0) {
		GST_ERROR("Unable to query encode allocation request %d", sts);
		return GST_MFX_ENCODER_STATUS_ERROR_ALLOCATION_FAILED;
	}

	sts = MFXVideoENCODE_Init(encoder->session, &encoder->params);
	if (sts < 0) {
		GST_ERROR("Error initializing the MFX video encoder %d", sts);
		return GST_MFX_ENCODER_STATUS_ERROR_OPERATION_FAILED;
	}

    /* Even if VPP is not required, surfaces need to be saved into a pool */
    if (!!(encoder->params.IOPattern & MFX_IOPATTERN_IN_VIDEO_MEMORY)) {
        encoder->filter = gst_mfx_filter_new_with_task(encoder->aggregator,
            encoder->encode_task, GST_MFX_TASK_VPP_OUT, encoder->mapped, FALSE);

        enc_request.NumFrameSuggested += (1 - encoder->params.AsyncDepth);

        gst_mfx_filter_set_request(encoder->filter, &enc_request,
            GST_MFX_TASK_VPP_OUT);

        gst_mfx_filter_set_frame_info(encoder->filter, &encoder->info);

        if (GST_VIDEO_INFO_FORMAT(&encoder->info) != GST_VIDEO_FORMAT_NV12)
            gst_mfx_filter_set_format(encoder->filter, GST_VIDEO_FORMAT_NV12);

        if (!gst_mfx_filter_start(encoder->filter))
            return GST_MFX_ENCODER_STATUS_ERROR_OPERATION_FAILED;
    }

	return GST_MFX_ENCODER_STATUS_SUCCESS;
}

GstMfxEncoderStatus
gst_mfx_encoder_encode(GstMfxEncoder * encoder, GstVideoCodecFrame * frame)
{
	GstMapInfo minfo;
	GstMfxSurfaceProxy *proxy, *filter_proxy;
	GstMfxFilterStatus filter_sts;
	mfxFrameSurface1 *insurf;
	mfxSyncPoint syncp;
	mfxStatus sts = MFX_ERR_NONE;

	proxy = gst_video_codec_frame_get_user_data(frame);

	if (gst_mfx_task_has_type(encoder->encode_task, GST_MFX_TASK_VPP_OUT)) {
        filter_sts = gst_mfx_filter_process(encoder->filter, proxy,
                            &filter_proxy);
        if (GST_MFX_FILTER_STATUS_SUCCESS != filter_sts) {
            GST_ERROR("MFX pre-processing error during encode.");
            return GST_MFX_ENCODER_STATUS_ERROR_OPERATION_FAILED;
        }
        proxy = filter_proxy;
	}

	insurf = gst_mfx_surface_proxy_get_frame_surface (proxy);

	do {
		sts = MFXVideoENCODE_EncodeFrameAsync(encoder->session,
			NULL, insurf, &encoder->bs, &syncp);

		if (MFX_WRN_DEVICE_BUSY == sts)
			g_usleep(500);
		else if (MFX_ERR_NOT_ENOUGH_BUFFER == sts) {
			encoder->bs.MaxLength += 1024 * 16;
			encoder->bitstream = g_byte_array_set_size(encoder->bitstream,
				encoder->bs.MaxLength);
            encoder->bs.Data = encoder->bitstream->data;
		}
	} while (MFX_WRN_DEVICE_BUSY == sts || MFX_ERR_NOT_ENOUGH_BUFFER == sts);

	if (MFX_ERR_MORE_BITSTREAM == sts)
		return GST_MFX_ENCODER_STATUS_NO_BUFFER;
	else if (MFX_ERR_MORE_DATA == sts)
		return GST_MFX_ENCODER_STATUS_MORE_DATA;

	if (sts != MFX_ERR_NONE &&
		sts != MFX_ERR_MORE_BITSTREAM &&
		sts != MFX_WRN_VIDEO_PARAM_CHANGED) {
		GST_ERROR("Error during MFX encoding.");
		return GST_MFX_ENCODER_STATUS_ERROR_UNKNOWN;
	}

	if (syncp) {
		do {
			sts = MFXVideoCORE_SyncOperation(encoder->session, syncp, 1000);
		} while (MFX_WRN_IN_EXECUTION == sts);

        frame->output_buffer =
            gst_buffer_new_wrapped_full (GST_MEMORY_FLAG_READONLY,
            encoder->bs.Data + encoder->bs.DataOffset,
            encoder->bs.MaxLength, 0, encoder->bs.DataLength, NULL, NULL);

        encoder->bs.DataLength = 0;
	}

	return GST_MFX_ENCODER_STATUS_SUCCESS;
}

GstMfxEncoderStatus
gst_mfx_encoder_flush(GstMfxEncoder * encoder, GstVideoCodecFrame ** frame)
{
    mfxSyncPoint syncp;
	mfxStatus sts = MFX_ERR_NONE;

    do {
		sts = MFXVideoENCODE_EncodeFrameAsync(encoder->session,
			NULL, NULL, &encoder->bs, &syncp);

		if (MFX_WRN_DEVICE_BUSY == sts)
			g_usleep(500);
        else if (MFX_ERR_NOT_ENOUGH_BUFFER == sts) {
			encoder->bs.MaxLength += 1024 * 16;
			encoder->bitstream = g_byte_array_set_size(encoder->bitstream,
				encoder->bs.MaxLength);
            encoder->bs.Data = encoder->bitstream->data;
		}
	} while (MFX_WRN_DEVICE_BUSY == sts || MFX_ERR_NOT_ENOUGH_BUFFER == sts);

	if (MFX_ERR_NONE != sts)
		return GST_MFX_ENCODER_STATUS_ERROR_OPERATION_FAILED;

	if (syncp) {
		do {
			sts = MFXVideoCORE_SyncOperation(encoder->session, syncp, 1000);
		} while (MFX_WRN_IN_EXECUTION == sts);

		if (MFX_ERR_NONE != sts)
            return GST_MFX_ENCODER_STATUS_ERROR_OPERATION_FAILED;

        *frame = g_slice_new0(GstVideoCodecFrame);

        (*frame)->output_buffer = gst_buffer_new_wrapped_full (GST_MEMORY_FLAG_READONLY,
                encoder->bs.Data + encoder->bs.DataOffset,
                encoder->bs.MaxLength, 0, encoder->bs.DataLength, NULL, NULL);

        encoder->bs.DataLength = 0;
	}

	return GST_MFX_ENCODER_STATUS_SUCCESS;
}

/**
* gst_mfx_encoder_set_property:
* @encoder: a #GstMfxEncoder
* @prop_id: the id of the property to change
* @value: the new value to set
*
* Update the requested property, designed by @prop_id, with the
* supplied @value. A @NULL value argument resets the property to its
* default value.
*
* Return value: a #GstMfxEncoderStatus
*/
static GstMfxEncoderStatus
set_property(GstMfxEncoder * encoder, gint prop_id, const GValue * value)
{
	GstMfxEncoderStatus status =
		GST_MFX_ENCODER_STATUS_ERROR_INVALID_PARAMETER;
    gboolean success = TRUE;

	g_assert(value != NULL);

	/* Handle codec-specific properties */
	if (prop_id < 0) {
		GstMfxEncoderClass *const klass = GST_MFX_ENCODER_GET_CLASS(encoder);

		if (klass->set_property) {
			status = klass->set_property(encoder, prop_id, value);
		}
		return status;
	}

	/* Handle common properties */
	switch (prop_id) {
	case GST_MFX_ENCODER_PROP_RATECONTROL:
		encoder->rc_method = g_value_get_enum(value);
		break;
	case GST_MFX_ENCODER_PROP_BITRATE:
	    encoder->bitrate = g_value_get_uint(value);
		break;
	case GST_MFX_ENCODER_PROP_IDR_INTERVAL:
	    encoder->idr_interval = g_value_get_uint(value);
		break;
	case GST_MFX_ENCODER_PROP_GOP_SIZE:
	    encoder->gop_size = g_value_get_uint(value);
		break;
	case GST_MFX_ENCODER_PROP_GOP_REFDIST:
		success = gst_mfx_encoder_set_gop_refdist(encoder,
			g_value_get_int(value));
		break;
	case GST_MFX_ENCODER_PROP_NUM_REFS:
		success = gst_mfx_encoder_set_num_references(encoder,
			g_value_get_uint(value));
		break;
	case GST_MFX_ENCODER_PROP_NUM_SLICES:
	    encoder->num_slices = g_value_get_uint(value);
		break;
    case GST_MFX_ENCODER_PROP_QUANTIZER:
		success = gst_mfx_encoder_set_quantizer(encoder,
            g_value_get_uint(value));
		break;
	case GST_MFX_ENCODER_PROP_QPI:
		success = gst_mfx_encoder_set_qpi_offset(encoder,
            g_value_get_uint(value));
		break;
	case GST_MFX_ENCODER_PROP_QPP:
		success = gst_mfx_encoder_set_qpp_offset(encoder,
            g_value_get_uint(value));
		break;
	case GST_MFX_ENCODER_PROP_QPB:
		success = gst_mfx_encoder_set_qpb_offset(encoder,
            g_value_get_uint(value));
		break;
    case GST_MFX_ENCODER_PROP_MBBRC:
		encoder->mbbrc = g_value_get_enum(value);
		break;
    case GST_MFX_ENCODER_PROP_EXTBRC:
		encoder->extbrc = g_value_get_enum(value);
		break;
    case GST_MFX_ENCODER_PROP_ADAPTIVE_I:
		encoder->adaptive_i = g_value_get_enum(value);
		break;
    case GST_MFX_ENCODER_PROP_ADAPTIVE_B:
		encoder->adaptive_b = g_value_get_enum(value);
		break;
    case GST_MFX_ENCODER_PROP_B_PYRAMID:
		encoder->b_strategy = g_value_get_enum(value);
		break;
    case GST_MFX_ENCODER_PROP_ACCURACY:
	    encoder->avbr_accuracy = g_value_get_uint(value);
		break;
    case GST_MFX_ENCODER_PROP_CONVERGENCE:
	    encoder->avbr_convergence = g_value_get_uint(value);
		break;
	case GST_MFX_ENCODER_PROP_PRESET:
		encoder->preset = g_value_get_enum(value);
		break;
    case GST_MFX_ENCODER_PROP_ASYNC_DEPTH:
		success = gst_mfx_encoder_set_async_depth(encoder,
            g_value_get_uint(value));
        break;
    default:
        success = FALSE;
		break;
	}

	return success ? GST_MFX_ENCODER_STATUS_SUCCESS :
        GST_MFX_ENCODER_STATUS_ERROR_INVALID_PARAMETER;
}

GstMfxEncoderStatus
gst_mfx_encoder_set_property(GstMfxEncoder * encoder, gint prop_id,
	const GValue * value)
{
	GstMfxEncoderStatus status = GST_MFX_ENCODER_STATUS_SUCCESS;
	GValue default_value = G_VALUE_INIT;

	g_return_val_if_fail(encoder != NULL,
		GST_MFX_ENCODER_STATUS_ERROR_INVALID_PARAMETER);

	if (!value) {
		GParamSpec *const pspec = prop_find_pspec(encoder, prop_id);
		if (!pspec)
			goto error_invalid_property;

		g_value_init(&default_value, pspec->value_type);
		g_param_value_set_default(pspec, &default_value);
		value = &default_value;
	}

	status = set_property(encoder, prop_id, value);

	if (default_value.g_type)
		g_value_unset(&default_value);
	return status;

	/* ERRORS */
error_invalid_property:
	{
		GST_ERROR("unsupported property (%d)", prop_id);
		return GST_MFX_ENCODER_STATUS_ERROR_INVALID_PARAMETER;
	}
}

/* Checks video info */
static GstMfxEncoderStatus
check_video_info(GstMfxEncoder * encoder, const GstVideoInfo * vip)
{
	if (!vip->width || !vip->height)
		goto error_invalid_resolution;
	if (vip->fps_n < 0 || vip->fps_d <= 0)
		goto error_invalid_framerate;
	return GST_MFX_ENCODER_STATUS_SUCCESS;

	/* ERRORS */
error_invalid_resolution:
	{
		GST_ERROR("invalid resolution (%dx%d)", vip->width, vip->height);
		return GST_MFX_ENCODER_STATUS_ERROR_INVALID_PARAMETER;
	}
error_invalid_framerate:
	{
		GST_ERROR("invalid framerate (%d/%d)", vip->fps_n, vip->fps_d);
		return GST_MFX_ENCODER_STATUS_ERROR_INVALID_PARAMETER;
	}
}


/* Reconfigures the encoder with the new properties */
static GstMfxEncoderStatus
gst_mfx_encoder_reconfigure_internal(GstMfxEncoder * encoder)
{
	GstMfxEncoderClass *const klass = GST_MFX_ENCODER_GET_CLASS(encoder);
	GstMfxEncoderStatus status;

	status = klass->reconfigure(encoder);
	if (status != GST_MFX_ENCODER_STATUS_SUCCESS)
		return status;

	return GST_MFX_ENCODER_STATUS_SUCCESS;
}

/**
* gst_mfx_encoder_set_codec_state:
* @encoder: a #GstMfxEncoder
* @state : a #GstVideoCodecState
*
* Notifies the encoder about the source surface properties. The
* accepted set of properties is: video resolution, colorimetry,
* pixel-aspect-ratio and framerate.
*
* This function is a synchronization point for codec configuration.
* This means that, at this point, the encoder is reconfigured to
* match the new properties and any other change beyond this point has
* zero effect.
*
* Return value: a #GstMfxEncoderStatus
*/
GstMfxEncoderStatus
gst_mfx_encoder_set_codec_state(GstMfxEncoder * encoder,
	GstVideoCodecState * state)
{
	GstMfxEncoderStatus status;

	g_return_val_if_fail(encoder != NULL,
		GST_MFX_ENCODER_STATUS_ERROR_INVALID_PARAMETER);
	g_return_val_if_fail(state != NULL,
		GST_MFX_ENCODER_STATUS_ERROR_INVALID_PARAMETER);

	if (!gst_video_info_is_equal(&state->info, &encoder->info)) {
		status = check_video_info(encoder, &state->info);
		if (status != GST_MFX_ENCODER_STATUS_SUCCESS)
			return status;
		encoder->info = state->info;
	}
	return gst_mfx_encoder_reconfigure_internal(encoder);

	/* ERRORS */
error_operation_failed:
	{
		GST_ERROR("could not change codec state after encoding started");
		return GST_MFX_ENCODER_STATUS_ERROR_OPERATION_FAILED;
	}
}

GstMfxEncoderStatus
gst_mfx_encoder_get_codec_data (GstMfxEncoder * encoder,
    GstBuffer ** out_codec_data_ptr)
{
  GstMfxEncoderStatus ret = GST_MFX_ENCODER_STATUS_SUCCESS;
  GstMfxEncoderClass *const klass = GST_MFX_ENCODER_GET_CLASS (encoder);

  *out_codec_data_ptr = NULL;
  if (!klass->get_codec_data)
    return GST_MFX_ENCODER_STATUS_SUCCESS;

  ret = klass->get_codec_data (encoder, out_codec_data_ptr);
  return ret;
}

GType
gst_mfx_encoder_preset_get_type (void)
{
    static volatile gsize g_type = 0;

    static const GEnumValue encoder_preset_values[] = {
        { GST_MFX_ENCODER_PRESET_VERY_FAST,
          "Best speed", "very-fast" },
        { GST_MFX_ENCODER_PRESET_FASTER,
          "Faster", "faster" },
        { GST_MFX_ENCODER_PRESET_FAST,
          "Fast", "fast" },
        { GST_MFX_ENCODER_PRESET_MEDIUM,
          "Balanced", "medium" },
        { GST_MFX_ENCODER_PRESET_SLOW,
          "Slow", "slow" },
        { GST_MFX_ENCODER_PRESET_SLOWER,
          "Slower", "slower" },
        { GST_MFX_ENCODER_PRESET_VERY_SLOW,
          "Best quality", "very-slow" },
        { 0, NULL, NULL },
  };

    if (g_once_init_enter (&g_type)) {
        GType type =
            g_enum_register_static ("GstMfxEncoderPreset", encoder_preset_values);
        g_once_init_leave (&g_type, type);
    }
    return g_type;
}

GType
gst_mfx_encoder_trellis_get_type (void)
{
    static volatile gsize g_type = 0;

    static const GEnumValue trellis_values[] = {
        { GST_MFX_ENCODER_TRELLIS_OFF,
          "Disable trellis", "off" },
        { GST_MFX_ENCODER_TRELLIS_I,
          "Enable trellis for I-frames", "i" },
        { GST_MFX_ENCODER_TRELLIS_IP,
          "Enable trellis for I/P-frames", "ip" },
        { GST_MFX_ENCODER_TRELLIS_IPB,
          "Enable trellis for I/P/B-frames", "ipb" },
        { 0, NULL, NULL },
    };

    if (g_once_init_enter (&g_type)) {
        GType type =
            g_enum_register_static ("GstMfxEncoderTrellis", trellis_values);
        g_once_init_leave (&g_type, type);
    }
    return g_type;
}

GType
gst_mfx_encoder_lookahead_ds_get_type (void)
{
    static volatile gsize g_type = 0;

    static const GEnumValue lookahead_ds_values[] = {
        { GST_MFX_ENCODER_LOOKAHEAD_DS_AUTO,
          "Let encoder decide", "auto" },
        { GST_MFX_ENCODER_LOOKAHEAD_DS_OFF,
          "No downsampling", "off" },
        { GST_MFX_ENCODER_LOOKAHEAD_DS_2X,
          "Downsample 2x", "2x" },
        { GST_MFX_ENCODER_LOOKAHEAD_DS_4X,
          "Downsample 4x", "4x" },
        { 0, NULL, NULL },
    };

    if (g_once_init_enter (&g_type)) {
        GType type =
            g_enum_register_static ("GstMfxEncoderLookAheadDS", lookahead_ds_values);
        g_once_init_leave (&g_type, type);
    }
    return g_type;
}
