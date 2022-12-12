// SPDX-License-Identifier: BSD-3-Clause
//
// Copyright(c) 2022 Intel Corporation. All rights reserved.

/* file component for reading/writing pcm samples to/from a file */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <errno.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <sof/sof.h>
#include <sof/list.h>
#include <sof/audio/stream.h>
#include <sof/audio/ipc-config.h>
#include <sof/ipc/driver.h>
#include <sof/audio/component.h>
#include <sof/audio/format.h>
#include <sof/audio/pipeline.h>
#include <ipc/stream.h>
#include <ipc/topology.h>
#include <ipc/dai.h>

#include <alsa/asoundlib.h>

/* 66def9f0-39f2-11ed-89f7-af98a6440cc4 */
DECLARE_SOF_RT_UUID("arecord", arecord_uuid, 0x66def9f0, 0x39f2, 0x11ed,
		    0xf7, 0x89, 0xaf, 0x98, 0xa6, 0x44, 0x0c, 0xc4);
DECLARE_TR_CTX(arecord_tr, SOF_UUID(arecord_uuid), LOG_LEVEL_INFO);

/* 72cee996-39f2-11ed-a08f-97fcc42eaaeb */
DECLARE_SOF_RT_UUID("aplay", aplay_uuid, 0x72cee996, 0x39f2, 0x11ed,
		    0xa0, 0x8f, 0x97, 0xfc, 0xc4, 0x2e, 0xaa, 0xeb);
DECLARE_TR_CTX(aplay_tr, SOF_UUID(aplay_uuid), LOG_LEVEL_INFO);

static const struct comp_driver comp_arecord;
static const struct comp_driver comp_aplay;

/* ALSA comp data */
struct alsa_comp_data {
	snd_pcm_t *handle;
	snd_pcm_info_t *info;
	snd_pcm_hw_params_t *hw_params;
	snd_pcm_sw_params_t *sw_params;
	snd_pcm_uframes_t period_size;
	snd_pcm_uframes_t buffer_size;
	char *pcm_name;
	struct sof_ipc_stream_params params;
};

static void alsa_free(struct comp_dev *dev)
{
	struct alsa_comp_data *cd = comp_get_drvdata(dev);

	printf("%s %d\n", __func__, __LINE__);

	snd_pcm_sw_params_free(cd->sw_params);
	snd_pcm_hw_params_free(cd->hw_params);
	snd_pcm_info_free(cd->info);
	snd_pcm_close(cd->handle);
	free(cd);
	free(dev);
}

static struct comp_dev *alsa_new(const struct comp_driver *drv,
				 const struct comp_ipc_config *config,
				 const void *spec)
{
	struct comp_dev *dev;
	struct alsa_comp_data *cd;
	int err;

	/* ALSA PCMs can only be of topology comp type DAI */
	if (config->type != SOF_COMP_DAI) {
		fprintf(stderr, "error: invalid comp type %d for ALSA PCM\n",
			config->type);
		return NULL;
	}

	dev = comp_alloc(drv, sizeof(*dev));
	if (!dev)
		return NULL;
	dev->ipc_config = *config;

	/* allocate  memory for file comp data */
	cd = rzalloc(SOF_MEM_ZONE_RUNTIME_SHARED, 0, SOF_MEM_CAPS_RAM, sizeof(*cd));
	if (!cd)
		goto error;

	comp_set_drvdata(dev, cd);

	/* get ALSA ready */
	err = snd_pcm_info_malloc(&cd->info);
	if (err < 0)
		goto error;

	err = snd_pcm_hw_params_malloc(&cd->hw_params);
	if (err < 0)
		goto error;

	err = snd_pcm_sw_params_malloc(&cd->sw_params);
	if (err < 0)
		goto error;

	/* TODO: HACK hard coded - get from cmd line and map */
	cd->pcm_name = "default";
	return dev;

error:
	free(cd);
	free(dev);
	return NULL;
}

static struct comp_dev *arecord_new(const struct comp_driver *drv,
				 const struct comp_ipc_config *config,
				 const void *spec)
{
	struct comp_dev *dev;
	struct alsa_comp_data *cd;
	struct sof_ipc_stream_params *params;
	int err;

	dev = alsa_new(drv, config, spec);
	if (!dev)
		return NULL;

	cd = comp_get_drvdata(dev);
	params = &cd->params;
	params->direction = SND_PCM_STREAM_CAPTURE;

	err = snd_pcm_open(&cd->handle, cd->pcm_name, SND_PCM_STREAM_CAPTURE, 0);
	if (err < 0) {
		fprintf(stderr, "error: cant open PCM: %s\n", snd_strerror(err));
		return NULL;
	}

	if ((err = snd_pcm_info(cd->handle, cd->info)) < 0) {
		fprintf(stderr, "error: cant get PCM info: %s\n", snd_strerror(err));
		return NULL;
	}

	/* TODO: set default config - get from topology/stream */
	params->rate = 48000;
	params->channels = 2;
	cd->buffer_size = 6000 * 4;//config->periods_source;
	cd->period_size = 6000;

	dev->state = COMP_STATE_READY;
	return dev;
}

static struct comp_dev *aplay_new(const struct comp_driver *drv,
				  const struct comp_ipc_config *config,
				  const void *spec)
{
	struct comp_dev *dev;
	struct alsa_comp_data *cd;
	struct sof_ipc_stream_params *params;
	int err;

	dev = alsa_new(drv, config, spec);
	if (!dev)
		return NULL;

	cd = comp_get_drvdata(dev);
	params = &cd->params;
	params->direction = SND_PCM_STREAM_PLAYBACK;

	err = snd_pcm_open(&cd->handle, cd->pcm_name, SND_PCM_STREAM_PLAYBACK, 0);
	if (err < 0) {
		fprintf(stderr, "error: cant open PCM: %s\n", snd_strerror(err));
		return NULL;
	}

	if ((err = snd_pcm_info(cd->handle, cd->info)) < 0) {
		fprintf(stderr, "error: cant get PCM info: %s\n", snd_strerror(err));
		return NULL;
	}

	dev->state = COMP_STATE_READY;

	/* TODO: set default config - get from topology/stream */
	params->rate = 48000;
	params->channels = 2;
	cd->buffer_size = 6000 * config->periods_sink;
	cd->period_size = 6000;

	return dev;
}

static int set_params(struct alsa_comp_data *cd)
{
	struct sof_ipc_stream_params *params = &cd->params;
	int frame_fmt;
	int err;
	int dir = params->direction;

	/* is sound card HW configuration valid ? */
	err = snd_pcm_hw_params_any(cd->handle, cd->hw_params);
	if (err < 0) {
		fprintf(stderr, "error: cant get PCM hw_params: %s\n", snd_strerror(err));
		return err;
	}

	/* set interleaved buffer format */
	err = snd_pcm_hw_params_set_access(cd->handle, cd->hw_params,
					   SND_PCM_ACCESS_RW_INTERLEAVED);
	if (err < 0) {
		fprintf(stderr, "error: PCM can't set interleaved: %s\n", snd_strerror(err));
		return err;
	}

	/* set sample format */
	/* set all topology configuration */
	switch (params->frame_fmt) {
	case SOF_IPC_FRAME_S16_LE:
		frame_fmt = SND_PCM_FORMAT_S16_LE;
		break;
	case SOF_IPC_FRAME_S24_4LE:
		frame_fmt = SND_PCM_FORMAT_S24_LE;
		break;
	case SOF_IPC_FRAME_S32_LE:
		frame_fmt = SND_PCM_FORMAT_S32_LE;
		break;
	case SOF_IPC_FRAME_FLOAT:
		frame_fmt = SND_PCM_FORMAT_FLOAT_LE;
		break;
	case SOF_IPC_FRAME_S24_3LE:
		frame_fmt = SND_PCM_FORMAT_S24_3LE;
		break;
	default:
		fprintf(stderr, "error: invalid frame format %d for ALSA PCM\n",
			params->frame_fmt);
		return -EINVAL;
	}
	err = snd_pcm_hw_params_set_format(cd->handle, cd->hw_params,
			frame_fmt);
	if (err < 0) {
		fprintf(stderr, "error: PCM can't set format %d: %s\n",
			frame_fmt, snd_strerror(err));
		return err;
	}

	/* set number of channels */
	err = snd_pcm_hw_params_set_channels(cd->handle, cd->hw_params,
			params->channels);
	if (err < 0) {
		fprintf(stderr, "error: PCM can't set channels %d: %s\n",
			params->channels, snd_strerror(err));
		return err;
	}

	/* set sample rate */
	err = snd_pcm_hw_params_set_rate(cd->handle, cd->hw_params, params->rate,
					 params->direction);
	if (err < 0) {
		fprintf(stderr, "error: PCM can't set rate %d: %s\n",
			params->rate, snd_strerror(err));
		return err;
	}

	/* commit the hw params */
	err = snd_pcm_hw_params(cd->handle, cd->hw_params);
	if (err < 0) {
		fprintf(stderr, "error: PCM can't commit hw_params: %s\n", snd_strerror(err));
		snd_pcm_hw_params_dump(cd->hw_params, SND_OUTPUT_STDIO);
		return err;
	}

	/* get period and buffer size - TODO: back propagate through pipeline */
	snd_pcm_hw_params_get_period_size(cd->hw_params, &cd->period_size,
			&dir);
	snd_pcm_hw_params_get_buffer_size(cd->hw_params, &cd->buffer_size);
	printf("%s got period size %ld buffer size %ld\n", __func__,
		cd->period_size, cd->buffer_size);

	/* get the initial SW params */
	err = snd_pcm_sw_params_current(cd->handle, cd->sw_params);
	if (err < 0) {
		fprintf(stderr, "error: PCM can't get sw params: %s\n", snd_strerror(err));
		return err;
	}

	/* set the avail min to the period size */
	err = snd_pcm_sw_params_set_avail_min(cd->handle, cd->sw_params, cd->period_size);
	if (err < 0) {
		fprintf(stderr, "error: PCM can't set avail min: %s\n", snd_strerror(err));
		return err;
	}

	/* PCM should start after receiving first periods worth of data */
	err = snd_pcm_sw_params_set_start_threshold(cd->handle, cd->sw_params, cd->period_size);
	if (err < 0) {
		fprintf(stderr, "error: PCM can't set start threshold: %s\n", snd_strerror(err));
		return err;
	}

	/* PCM should stop if only 1/4 period worth of data is available */
	err = snd_pcm_sw_params_set_stop_threshold(cd->handle, cd->sw_params, cd->period_size / 4);
	if (err < 0) {
		fprintf(stderr, "error: PCM can't set stop threshold: %s\n", snd_strerror(err));
		return err;
	}

	/* commit the sw params */
	if (snd_pcm_sw_params(cd->handle, cd->sw_params) < 0) {
		fprintf(stderr, "error: PCM can't commit sw_params: %s\n", snd_strerror(err));
		snd_pcm_sw_params_dump(cd->sw_params, SND_OUTPUT_STDIO);
		return err;
	}

	return 0;
}

/**
 * \brief Sets file component audio stream parameters.
 * \param[in,out] dev Volume base component device.
 * \param[in] params Audio (PCM) stream parameters (ignored for this component)
 * \return Error code.
 *
 * All done in prepare() since we need to know source and sink component params.
 */
static int arecord_params(struct comp_dev *dev,
		       struct sof_ipc_stream_params *params)
{
	struct comp_buffer *buffer;
	struct alsa_comp_data *cd = comp_get_drvdata(dev);
	int ret;

	comp_dbg(dev, "params");

	if (params->direction != SND_PCM_STREAM_CAPTURE) {
		comp_err(dev, "alsa_params(): pcm params invalid direction.");
		return -EINVAL;
	}

	/* params can be aligned to match pipeline here */
	ret = comp_verify_params(dev, 0, params);
	if (ret < 0) {
		comp_err(dev, "alsa_params(): pcm params verification failed.");
		return ret;
	}
	memcpy(&cd->params, params, sizeof(*params));

	/* file component sink/source buffer period count */
	buffer = list_first_item(&dev->bsink_list, struct comp_buffer,
				 source_list);
	buffer_reset_pos(buffer, NULL);

	return set_params(cd);
}

static int aplay_params(struct comp_dev *dev,
		       struct sof_ipc_stream_params *params)
{
	struct comp_buffer *buffer;
	struct alsa_comp_data *cd = comp_get_drvdata(dev);
	int ret;

	comp_dbg(dev, "params");

	if (params->direction != SND_PCM_STREAM_PLAYBACK) {
		comp_err(dev, "alsa_params(): pcm params invalid direction.");
		return -EINVAL;
	}

	/* params can be aligned to match pipeline here */
	ret = comp_verify_params(dev, 0, params);
	if (ret < 0) {
		comp_err(dev, "alsa_params(): pcm params verification failed.");
		return ret;
	}
	memcpy(&cd->params, params, sizeof(*params));

	/* file component sink/source buffer period count */
	buffer = list_first_item(&dev->bsource_list, struct comp_buffer,
				 sink_list);
	buffer_reset_pos(buffer, NULL);

	return set_params(cd);
}

static int alsa_trigger(struct comp_dev *dev, int cmd)
{
	/* trigger is handled automatically by ALSA start threshold */
	comp_dbg(dev, "trigger cmd %d", cmd);
	return comp_set_state(dev, cmd);
}

/* used to pass standard and bespoke commands (with data) to component */
static int alsa_cmd(struct comp_dev *dev, int cmd, void *data,
		    int max_data_size)
{
	return 0;
}

/*
 * copy and process stream samples
 * returns the number of bytes copied
 */
static int arecord_copy(struct comp_dev *dev)
{
	struct alsa_comp_data *cd = comp_get_drvdata(dev);
	struct comp_buffer *buffer;
	struct audio_stream *sink;
	snd_pcm_sframes_t frames;
	snd_pcm_uframes_t free;
	snd_pcm_uframes_t total = 0;
	unsigned frame_bytes;
	void *pos;

	/* file component sink buffer */
	buffer = list_first_item(&dev->bsink_list, struct comp_buffer,
				 source_list);
	sink = &buffer->stream;
	pos = sink->w_ptr;
	free = audio_stream_get_free_frames(sink);
	frame_bytes = audio_stream_frame_bytes(sink);

	while (free) {
		frames = MIN(free, audio_stream_frames_without_wrap(sink, pos));

		/* read PCM samples from file */
		frames = snd_pcm_readi(cd->handle, pos, frames);
		if (frames < 0) {
			fprintf(stderr, "failed to read: %s: %s\n",
				cd->pcm_name, snd_strerror(frames));
			return frames;
		}

		free -= frames;
		pos = audio_stream_wrap(sink, pos + frames * frame_bytes);
		total += frames;
	}

	/* update sink buffer pointers */
	comp_update_buffer_produce(buffer, total * frame_bytes);
	comp_dbg(dev, "read %d frames", total);

	return 0;
}

/*
 * copy and process stream samples
 * returns the number of bytes copied
 */
static int aplay_copy(struct comp_dev *dev)
{
	struct alsa_comp_data *cd = comp_get_drvdata(dev);
	struct comp_buffer *buffer;
	struct audio_stream *source;
	snd_pcm_sframes_t frames;
	snd_pcm_sframes_t avail;
	snd_pcm_uframes_t total = 0;
	unsigned frame_bytes;
	void *pos;

	/* file component source buffer */
	buffer = list_first_item(&dev->bsource_list, struct comp_buffer,
				 sink_list);
	source = &buffer->stream;
	pos = source->r_ptr;
	avail = audio_stream_get_avail_frames(source);
	frame_bytes = audio_stream_frame_bytes(source);

	while (avail > 0) {

		frames = MIN(avail, audio_stream_frames_without_wrap(source, pos));

		/* write PCM samples to PCM */
		frames = snd_pcm_writei(cd->handle, pos, frames);
		if (frames < 0) {
			fprintf(stderr, "failed to write: %s: %s\n",
				cd->pcm_name, snd_strerror(frames));
			return frames;
		}

		avail -= frames;
		pos = audio_stream_wrap(source, pos + frames * frame_bytes);
		total += frames;
	}

	/* update sink buffer pointers */
	comp_update_buffer_consume(buffer, total * frame_bytes);
	comp_dbg(dev, "wrote %d frames", total);

	return 0;
}

static int alsa_prepare(struct comp_dev *dev)
{
	int ret = 0;

	comp_dbg(dev, "prepare");
	ret = comp_set_state(dev, COMP_TRIGGER_PREPARE);
	if (ret < 0)
		return ret;

	if (ret == COMP_STATUS_STATE_ALREADY_SET)
		return PPL_STATUS_PATH_STOP;

	return ret;
}

static int alsa_reset(struct comp_dev *dev)
{
	/* TODO: reset to known state */
	comp_dbg(dev, "reset");
	comp_set_state(dev, COMP_TRIGGER_RESET);
	return 0;
}

/*
 * TODO: we pass the DAI topology config back up the pipeline so
 * that upstream/downstream can be configured. Needs to be configured
 * at stream runtime instead of at topology load time.
 */
static int alsa_dai_get_hw_params(struct comp_dev *dev,
		 struct sof_ipc_stream_params *params,
		 int dir)
{
	struct alsa_comp_data *cd = comp_get_drvdata(dev);

	memcpy(params, &cd->params, sizeof(*params));

	comp_dbg(dev, "rate %d", params->rate);
	comp_dbg(dev, "frame format %d", params->frame_fmt);
	comp_dbg(dev, "channels %d", params->channels);
	comp_dbg(dev, "buffer size", params->buffer.size);

	return 0;
}

static const struct comp_driver comp_arecord = {
	.type = SOF_COMP_FILEREAD,
	.uid = SOF_RT_UUID(arecord_uuid),
	.tctx	= &arecord_tr,
	.ops = {
		.create = arecord_new,
		.free = alsa_free,
		.params = arecord_params,
		.cmd = alsa_cmd,
		.trigger = alsa_trigger,
		.copy = arecord_copy,
		.prepare = alsa_prepare,
		.reset = alsa_reset,
		.dai_get_hw_params = alsa_dai_get_hw_params,
	},
};

static const struct comp_driver comp_aplay = {
	.type = SOF_COMP_FILEWRITE,
	.uid = SOF_RT_UUID(aplay_uuid),
	.tctx	= &aplay_tr,
	.ops = {
		.create = aplay_new,
		.free = alsa_free,
		.params = aplay_params,
		.cmd = alsa_cmd,
		.trigger = alsa_trigger,
		.copy = aplay_copy,
		.prepare = alsa_prepare,
		.reset = alsa_reset,
		.dai_get_hw_params = alsa_dai_get_hw_params,
	},
};

static struct comp_driver_info comp_arecord_info = {
	.drv = &comp_arecord,
};

static struct comp_driver_info comp_aplay_info = {
	.drv = &comp_aplay,
};

static void sys_comp_alsa_init(void)
{
	comp_register(&comp_arecord_info);
	comp_register(&comp_aplay_info);
}

DECLARE_MODULE(sys_comp_alsa_init);
