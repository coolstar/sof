// SPDX-License-Identifier: BSD-3-Clause
//
// Copyright(c) 2022 Intel Corporation. All rights reserved.
//
// Author: Liam Girdwood <liam.r.girdwood@linux.intel.com>

#include <stdio.h>
#include <sys/poll.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <mqueue.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <semaphore.h>
#include <assert.h>
#include <errno.h>

#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>

#include <sof/sof.h>
#include <sof/audio/pipeline.h>
#include <sof/audio/component.h>
#include <ipc/stream.h>
#include <tplg_parser/topology.h>

#include "plugin.h"
#include "common.h"

typedef struct snd_sof_pcm {
	snd_pcm_ioplug_t io;
	size_t frame_size;
	struct timespec wait_timeout;
	int capture;
	int events;

	/* PCM flow control */
	struct plug_sem_desc ready;
	struct plug_sem_desc done;

	struct plug_shm_desc shm_ctx;
	struct plug_shm_desc shm_pcm;

	struct plug_mq_desc ipc;

} snd_sof_pcm_t;

static int plug_pcm_start(snd_pcm_ioplug_t * io)
{
	snd_sof_plug_t *plug = io->private_data;
	snd_sof_pcm_t *pcm = plug->module_prv;
	struct plug_shm_endpoint *ctx = pcm->shm_pcm.addr;
	struct sof_ipc_stream stream = {0};
	int err;

	switch (ctx->state) {
	case SOF_PLUGIN_STATE_READY:
		/* trigger start */
		stream.hdr.size = sizeof(stream);
		stream.hdr.cmd = SOF_IPC_GLB_STREAM_MSG | SOF_IPC_STREAM_TRIG_START;
		stream.comp_id = ctx->comp_id;

		err = plug_mq_cmd(&pcm->ipc, &stream, sizeof(stream), &stream, sizeof(stream));
		if (err < 0) {
			SNDERR("error: can't trigger START the PCM\n");
			return err;
		}

		// HACK - start the read for capture
		if (pcm->capture)
			ctx->total = 6000;
		break;
	case SOF_PLUGIN_STATE_STREAM_RUNNING:
		/* do nothing */
		break;
	case SOF_PLUGIN_STATE_INIT:
	case SOF_PLUGIN_STATE_STREAM_ERROR:
	case SOF_PLUGIN_STATE_DEAD:
	default:
		/* some error */
		SNDERR("pcm start: invalid pipe state: %d", ctx->state);
		return -EINVAL;
	}

	return 0;
}

static int plug_pcm_stop(snd_pcm_ioplug_t * io)
{
	snd_sof_plug_t *plug = io->private_data;
	snd_sof_pcm_t *pcm = plug->module_prv;
	struct plug_shm_endpoint *ctx = pcm->shm_pcm.addr;
	struct sof_ipc_stream stream = {0};
	int err;

	switch (ctx->state) {
	case SOF_PLUGIN_STATE_READY:
		/* do nothing */
		break;
	case SOF_PLUGIN_STATE_STREAM_RUNNING:
		stream.hdr.size = sizeof(stream);
		stream.hdr.cmd = SOF_IPC_GLB_STREAM_MSG | SOF_IPC_STREAM_TRIG_STOP;
		stream.comp_id = ctx->comp_id;

		err = plug_mq_cmd(&pcm->ipc, &stream, sizeof(stream), &stream, sizeof(stream));
		if (err < 0) {
			SNDERR("error: can't trigger STOP the PCM\n");
			return err;
		}
		break;
	case SOF_PLUGIN_STATE_INIT:
	case SOF_PLUGIN_STATE_STREAM_ERROR:
	case SOF_PLUGIN_STATE_DEAD:
	default:
		/* some error */
		SNDERR("pcm stop: invalid pipe state: %d", ctx->state);
		return -EINVAL;
	}

	return 0;
}

static int plug_pcm_drain(snd_pcm_ioplug_t * io)
{
	snd_sof_plug_t *plug = io->private_data;
	int err = 0;

	fprintf(stderr, "%s %d\n", __func__, __LINE__);
	return err;
}

/* buffer position up to buffer_size */
static snd_pcm_sframes_t plug_pcm_pointer(snd_pcm_ioplug_t *io)
{
	snd_sof_plug_t *plug = io->private_data;
	snd_sof_pcm_t *pcm = plug->module_prv;
	struct plug_shm_endpoint *ctx = pcm->shm_pcm.addr;
	snd_pcm_sframes_t ret = 0;
	int err;

	if (io->state == SND_PCM_STATE_XRUN)
		return -EPIPE;

	if (io->state != SND_PCM_STATE_RUNNING)
		return 0;

	switch (ctx->state) {
	case SOF_PLUGIN_STATE_STREAM_RUNNING:
		return ctx->total;
	case SOF_PLUGIN_STATE_READY:
	case SOF_PLUGIN_STATE_INIT:
	case SOF_PLUGIN_STATE_STREAM_ERROR:
	case SOF_PLUGIN_STATE_DEAD:
	default:
		/* some error */
		SNDERR("pcm stop: invalid pipe state: %d", ctx->state);
		return -EPIPE;
	}
}

/* get the delay for the running PCM; optional; since v1.0.1 */
static int plug_pcm_delay(snd_pcm_ioplug_t * io, snd_pcm_sframes_t * delayp)
{
	snd_sof_plug_t *plug = io->private_data;
	snd_sof_pcm_t *pcm = plug->module_prv;
	struct plug_shm_endpoint *ctx = pcm->shm_pcm.addr;
	int err = 0;

	fprintf(stderr, "%s %d\n", __func__, __LINE__);

	switch (ctx->state) {
	case SOF_PLUGIN_STATE_STREAM_RUNNING:
		*delayp = 0;
		return 0;
	case SOF_PLUGIN_STATE_STREAM_ERROR:
		snd_pcm_ioplug_set_state(io, SND_PCM_STATE_XRUN);
		return 0;
	case SOF_PLUGIN_STATE_READY:
	case SOF_PLUGIN_STATE_INIT:
	case SOF_PLUGIN_STATE_DEAD:
	default:
		/* some error */
		SNDERR("pcm stop: invalid pipe state: %d", ctx->state);
		return -EPIPE;
	}
}

/* return frames written */
static snd_pcm_sframes_t plug_pcm_write(snd_pcm_ioplug_t *io,
				     const snd_pcm_channel_area_t *areas,
				     snd_pcm_uframes_t offset,
				     snd_pcm_uframes_t size)
{
	snd_sof_plug_t *plug = io->private_data;
	snd_sof_pcm_t *pcm = plug->module_prv;
	struct plug_shm_endpoint *ctx = pcm->shm_pcm.addr;
	snd_pcm_sframes_t ret = 0;
	ssize_t bytes;
	const char *buf;
	int err;

	/* check the ctx->state here - run the pipeline if its not active */
	switch (ctx->state) {
	case SOF_PLUGIN_STATE_INIT:
	case SOF_PLUGIN_STATE_STREAM_ERROR:
	case SOF_PLUGIN_STATE_DEAD:
		/* some error */
		SNDERR("write: invalid pipe state: %d", ctx->state);
		return -EINVAL;
	case SOF_PLUGIN_STATE_READY:
		/* trigger start */
		err = plug_pcm_start(io);
		if (err < 0) {
			SNDERR("write: cant start pipe: %d", err);
			return err;
		}
		break;
	case SOF_PLUGIN_STATE_STREAM_RUNNING:
	default:
		/* do nothing */
		break;
	}

	/* calculate the buffer position and size */
	buf = (char *)areas->addr + (areas->first + areas->step * offset) / 8;
	bytes = size * pcm->frame_size;

	/* write audio data to pipe */
	memcpy(plug_ep_wptr(ctx), buf, bytes);
	plug_ep_produce(ctx, bytes);

	/* tell the pipe data is ready */
	sem_post(pcm->ready.sem);

	/* wait for sof-pipe reader to consume data or timeout */
	err = clock_gettime(CLOCK_REALTIME, &pcm->wait_timeout);
	if (err == -1) {
		SNDERR("write: cant get time: %s", strerror(errno));
		return -EPIPE;
	}

	plug_timespec_add_ms(&pcm->wait_timeout, 200);

	err = sem_timedwait(pcm->done.sem, &pcm->wait_timeout);
	if (err == -1) {
		SNDERR("write: fatal timeout: %s", strerror(errno));
		return -EPIPE;
	}

	return bytes / pcm->frame_size;
}

/* return frames read */
static snd_pcm_sframes_t plug_pcm_read(snd_pcm_ioplug_t *io,
				    const snd_pcm_channel_area_t *areas,
				    snd_pcm_uframes_t offset,
				    snd_pcm_uframes_t size)
{
	snd_sof_plug_t *plug = io->private_data;
	snd_sof_pcm_t *pcm = plug->module_prv;
	struct plug_shm_endpoint *ctx = pcm->shm_pcm.addr;
	snd_pcm_sframes_t ret = 0;
	ssize_t bytes;
	char *buf;
	int err;

	/* check the ctx->state here - run the pipeline if its not active */
	switch (ctx->state) {
	case SOF_PLUGIN_STATE_INIT:
	case SOF_PLUGIN_STATE_STREAM_ERROR:
	case SOF_PLUGIN_STATE_DEAD:
		/* some error */
		SNDERR("write: invalid pipe state: %d", ctx->state);
		return -EINVAL;
	case SOF_PLUGIN_STATE_READY:
		/* trigger start */
		err = plug_pcm_start(io);
		if (err < 0) {
			SNDERR("write: cant start pipe: %d", err);
			return err;
		}
		break;
	case SOF_PLUGIN_STATE_STREAM_RUNNING:
	default:
		/* do nothing */
		break;
	}

	/* tell the pipe data is ready */
	sem_post(pcm->ready.sem);

	/* calculate the buffer position and size */
	buf = (char *)areas->addr + (areas->first + areas->step * offset) / 8;

	/* wait for sof-pipe reader to consume data or timeout */
	err = clock_gettime(CLOCK_REALTIME, &pcm->wait_timeout);
	if (err == -1) {
		SNDERR("write: cant get time: %s", strerror(errno));
		return -EPIPE;
	}

	plug_timespec_add_ms(&pcm->wait_timeout, 200);

	/* wait for sof-pipe writer to produce data or timeout */
	err = sem_timedwait(pcm->done.sem, &pcm->wait_timeout);
	if (err == -1) {
		SNDERR("read: fatal timeout: %s", strerror(errno));
		return -EPIPE;
	}

	bytes = MIN(plug_ep_get_avail(ctx), size * pcm->frame_size);

	/* write audio data to pipe */
	memcpy(buf, plug_ep_rptr(ctx), bytes);
	plug_ep_consume(ctx, bytes);

	return bytes / pcm->frame_size;
}

static int plug_pcm_prepare(snd_pcm_ioplug_t * io)
{
	snd_sof_plug_t *plug = io->private_data;
	int err = 0;

	return err;
}

static int plug_pcm_hw_params(snd_pcm_ioplug_t * io,
			   snd_pcm_hw_params_t * params)
{
	snd_sof_plug_t *plug = io->private_data;
	snd_sof_pcm_t *pcm = plug->module_prv;
	struct plug_shm_endpoint *ctx = pcm->shm_pcm.addr;
	struct sof_ipc_pcm_params ipc_params = {0};
	struct sof_ipc_pcm_params_reply params_reply = {0};
	struct ipc_comp_dev *pcm_dev;
	struct comp_dev *cd;
	int err = 0;

	/* set plug params */
	ipc_params.comp_id = ctx->comp_id;
	ipc_params.params.buffer_fmt = SOF_IPC_BUFFER_INTERLEAVED; // TODO:
	ipc_params.params.rate = io->rate;
	ipc_params.params.channels = io->channels;

	switch (io->format) {
	case SND_PCM_FORMAT_S16_LE:
		ipc_params.params.frame_fmt = SOF_IPC_FRAME_S16_LE;
		ipc_params.params.sample_container_bytes = 2;
		ipc_params.params.sample_valid_bytes = 2;
		break;
	case SND_PCM_FORMAT_S24_LE:
		ipc_params.params.frame_fmt = SOF_IPC_FRAME_S24_4LE;
		ipc_params.params.sample_container_bytes = 4;
		ipc_params.params.sample_valid_bytes = 3;
		break;
	case SND_PCM_FORMAT_S32_LE:
		ipc_params.params.frame_fmt = SOF_IPC_FRAME_S32_LE;
		ipc_params.params.sample_container_bytes = 4;
		ipc_params.params.sample_valid_bytes = 4;
		break;
	default:
		SNDERR("SOF: Unsupported format %s\n",
			snd_pcm_format_name(io->format));
		return -EINVAL;
	}

	pcm->frame_size = ctx->frame_size =
	    (snd_pcm_format_physical_width(io->format) * io->channels) / 8;

	ipc_params.params.host_period_bytes = io->period_size * pcm->frame_size;

	/* Set pipeline params direction from scheduling component */
	ipc_params.params.direction = io->stream;
	ipc_params.params.hdr.size = sizeof(ipc_params.params);
	ipc_params.hdr.size = sizeof(ipc_params);
	ipc_params.hdr.cmd = SOF_IPC_GLB_STREAM_MSG | SOF_IPC_STREAM_PCM_PARAMS;

	err = plug_mq_cmd(&pcm->ipc, &ipc_params, sizeof(ipc_params),
			   &params_reply, sizeof(params_reply));
	if (err < 0) {
		SNDERR("error: can't set PCM params\n");
		return err;
	}

	if (params_reply.rhdr.error) {
		SNDERR("error: hw_params failed");
		err = -EINVAL;
	}

	/* needs to be set here and NOT in SW params as SW params not
	 * called in capture flow ? */
	ctx->buffer_size = io->buffer_size * ctx->frame_size;

	return err;
}

// TODO: why not called for arecord
static int plug_pcm_sw_params(snd_pcm_ioplug_t *io, snd_pcm_sw_params_t *params)
{
	snd_sof_plug_t *plug = io->private_data;
	snd_sof_pcm_t *pcm = plug->module_prv;
	snd_pcm_uframes_t start_threshold;
	struct plug_shm_endpoint *ctx = pcm->shm_pcm.addr;
	int err;

	/* get the stream start threshold */
	err = snd_pcm_sw_params_get_start_threshold(params, &start_threshold);
	if (err < 0) {
		SNDERR("sw params: failed to get start threshold: %s", strerror(err));
		return err;
	}

	/* TODO: this seems to be ignored or overridden by application params ??? */
	if (start_threshold < io->period_size) {

		start_threshold = io->period_size;
		err = snd_pcm_sw_params_set_start_threshold(pcm->io.pcm,
							    params, start_threshold);
		if (err < 0) {
			SNDERR("sw params: failed to set start threshold %d: %s",
				start_threshold, strerror(err));
			return err;
		}
	}

	/* keep running as long as we can */
	err = snd_pcm_sw_params_set_avail_min(pcm->io.pcm, params, 1);
	if (err < 0) {
		SNDERR("sw params: failed to set avail min %d: %s",
			1, strerror(err));
		return err;
	}

	return 0;
}

static int plug_pcm_close(snd_pcm_ioplug_t * io)
{
	snd_sof_plug_t *plug = io->private_data;
	snd_sof_pcm_t *pcm = plug->module_prv;
	struct plug_shm_glb_state *ctx = pcm->shm_ctx.addr;
	int err;

	return err;
}

static const snd_pcm_ioplug_callback_t sof_playback_callback = {
	.start = plug_pcm_start,
	.stop = plug_pcm_stop,
	.drain = plug_pcm_drain,
	.pointer = plug_pcm_pointer,
	.transfer = plug_pcm_write,
	.delay = plug_pcm_delay,
	.prepare = plug_pcm_prepare,
	.hw_params = plug_pcm_hw_params,
	.sw_params = plug_pcm_sw_params,
	.close = plug_pcm_close,
};

static const snd_pcm_ioplug_callback_t sof_capture_callback = {
	.start = plug_pcm_start,
	.stop = plug_pcm_stop,
	.pointer = plug_pcm_pointer,
	.transfer = plug_pcm_read,
	.delay = plug_pcm_delay,
	.prepare = plug_pcm_prepare,
	.hw_params = plug_pcm_hw_params,
	.close = plug_pcm_close,
};

static const snd_pcm_access_t access_list[] = {
	SND_PCM_ACCESS_RW_INTERLEAVED
};

static const unsigned int formats[] = {
	SND_PCM_FORMAT_S16_LE,
	SND_PCM_FORMAT_FLOAT_LE,
	SND_PCM_FORMAT_S32_LE,
	SND_PCM_FORMAT_S24_LE,
};

/*
 * Set HW constraints for the SOF plugin. This needs to be quite unrestrictive atm
 * as we really need to parse topology before the HW constraints can be narrowed
 * to a range that will work with the specified pipeline.
 * TODO: Align with topology.
 */
static int plug_hw_constraint(snd_sof_plug_t * plug)
{
	snd_sof_pcm_t *pcm = plug->module_prv;
	snd_pcm_ioplug_t *io = &pcm->io;
	int err;

	err = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_ACCESS,
					    ARRAY_SIZE(access_list),
					    access_list);
	if (err < 0) {
		SNDERR("constraints: failed to set access: %s", strerror(err));
		return err;
	}

	err = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_FORMAT,
					    ARRAY_SIZE(formats), formats);
	if (err < 0) {
		SNDERR("constraints: failed to set format: %s", strerror(err));
		return err;
	}

	err =
	    snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_CHANNELS,
					    1, 8);
	if (err < 0) {
		SNDERR("constraints: failed to set channels: %s", strerror(err));
		return err;
	}

	err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_RATE,
					      1, 192000);
	if (err < 0) {
		SNDERR("constraints: failed to set rate: %s", strerror(err));
		return err;
	}

	err =
	    snd_pcm_ioplug_set_param_minmax(io,
					    SND_PCM_IOPLUG_HW_BUFFER_BYTES,
					    1, 4 * 1024 * 1024);
	if (err < 0) {
		SNDERR("constraints: failed to set buffer bytes: %s", strerror(err));
		return err;
	}

	err =
	    snd_pcm_ioplug_set_param_minmax(io,
					    SND_PCM_IOPLUG_HW_PERIOD_BYTES,
					    128, 2 * 1024 * 1024);
	if (err < 0) {
		SNDERR("constraints: failed to set period bytes: %s", strerror(err));
		return err;
	}

	err =
	    snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_PERIODS,
					   1, 4);
	if (err < 0) {
		SNDERR("constraints: failed to set period count: %s", strerror(err));
		return err;
	}

	return 0;
}

/*
 * Register the plugin with ALSA and make available for use.
 * TODO: setup all audio params
 * TODO: setup polling fd for RW or mmap IOs
 */
static int plug_create(snd_sof_plug_t *plug, snd_pcm_t **pcmp, const char *name,
		       snd_pcm_stream_t stream, int mode)
{
	snd_sof_pcm_t *pcm = plug->module_prv;
	int err;

	pcm->io.version = SND_PCM_IOPLUG_VERSION;
	pcm->io.name = "ALSA <-> SOF PCM I/O Plugin";
	pcm->io.poll_fd = pcm->shm_pcm.fd;
	pcm->io.poll_events = POLLIN;
	pcm->io.mmap_rw = 0;

	if (stream == SND_PCM_STREAM_PLAYBACK) {
		pcm->io.callback = &sof_playback_callback;
	} else {
		pcm->io.callback = &sof_capture_callback;
	}
	pcm->io.private_data = plug;

	/* create the plugin */
	err = snd_pcm_ioplug_create(&pcm->io, name, stream, mode);
	if (err < 0) {
		SNDERR("failed to register plugin %s: %s\n", name, strerror(err));
		return err;
	}

	/* set the HW constrainst */
	err = plug_hw_constraint(plug);
	if (err < 0) {
		snd_pcm_ioplug_delete(&pcm->io);
		return err;
	}

	*pcmp = pcm->io.pcm;
	return 0;
}

/*
 * Complete parent initialisation.
 * 1. Check if pipe already ready by opening SHM context and IPC.
 * 2. TODO: check context state and load topology is needed for core.
 * 3. Open SHM and locks for this PCM plugin.
 */
static int plug_init_sof_pipe(snd_sof_plug_t *plug, snd_pcm_t **pcmp,
							  const char *name, snd_pcm_stream_t stream,
							  int mode)
{
	snd_sof_pcm_t *pcm = plug->module_prv;
	struct timespec delay;
	struct plug_shm_glb_state *ctx;
	int err;

	/* init name for PCM ready lock */
	err = plug_lock_init(&pcm->ready, plug->tplg_file, "ready",
				plug->tplg_pcm);
	if (err < 0) {
		SNDERR("error: invalid name for PCM ready lock %s\n",
				plug->tplg_file);
		return err;
	}

	/* init name for PCM done lock */
	err = plug_lock_init(&pcm->done, plug->tplg_file, "done",
				plug->tplg_pcm);
	if (err < 0) {
		SNDERR("error: invalid name for PCM done lock %s\n",
				plug->tplg_file);
		return err;
	}

	/* init IPC message queue name */
	err = plug_mq_init(&pcm->ipc, plug->tplg_file, "pcm", plug->tplg_pcm);
	if (err < 0) {
		SNDERR("error: invalid name for IPC mq %s\n", plug->tplg_file);
		return err;
	}

	/* init global status shm name */
	err = plug_shm_init(&pcm->shm_ctx, plug->tplg_file, "ctx", 0);
	if (err < 0) {
		SNDERR("error: invalid name for global SHM %s\n", plug->tplg_file);
		return err;
	}

	/* init PCM shm name */
	err = plug_shm_init(&pcm->shm_pcm, plug->tplg_file, "pcm",
				plug->tplg_pcm);
	if (err < 0) {
		SNDERR("error: invalid name for PCM SHM %s\n", plug->tplg_file);
		return err;
	}

	/* open the global sof-pipe context via SHM */
	pcm->shm_ctx.size = 128 * 1024;
	err = plug_shm_open(&pcm->shm_ctx);
	if (err < 0) {
		SNDERR("error: failed to open sof-pipe context: %s:%s",
			pcm->shm_ctx.name, strerror(err));
		return -errno;
	}

	/* open the sof-pipe IPC message queue */
	err = plug_mq_open(&pcm->ipc);
	if (err < 0) {
		SNDERR("error: failed to open sof-pipe IPC mq %s: %s",
				pcm->ipc.queue_name, strerror(err));
		return -errno;
	}

	/* open lock "ready" for PCM audio data */
	err = plug_lock_open(&pcm->ready);
	if (err < 0) {
		SNDERR("error: failed to open sof-pipe ready lock %s: %s",
				pcm->ready.name, strerror(err));
		return -errno;
	}

	/* open lock "done" for PCM audio data */
	err = plug_lock_open(&pcm->done);
	if (err < 0) {
		SNDERR("error: failed to open sof-pipe done lock %s: %s",
				pcm->done.name, strerror(err));
		return -errno;
	}

	/* open audio PCM SHM data endpoint */
	err = plug_shm_open(&pcm->shm_pcm);
	if (err < 0) {
		SNDERR("error: failed to open sof-pipe PCM SHM %s: %s",
				pcm->shm_pcm.name, strerror(err));
		return -errno;
	}

	/* now register the plugin */
	err = plug_create(plug, pcmp, name, stream, mode);
	if (err < 0) {
		SNDERR("failed to create plugin: %s", strerror(err));
		return -errno;
	}

	return 0;
}

/*
 * ALSA PCM plugin entry point.
 */
SND_PCM_PLUGIN_DEFINE_FUNC(sof)
{
	snd_sof_plug_t *plug;
	snd_sof_pcm_t *pcm;
	int err;

	/* create context */
	plug = calloc(1, sizeof(*plug));
	if (!plug)
		return -ENOMEM;


	pcm = calloc(1, sizeof(*pcm));
	if (!pcm) {
		free(plug);
		return -ENOMEM;
	}
	plug->module_prv = pcm;

	if (stream == SND_PCM_STREAM_CAPTURE)
		pcm->capture = 1;

	/* parse the ALSA configuration file for sof plugin */
	err = plug_parse_conf(plug, name, root, conf);
	if (err < 0) {
		SNDERR("failed to parse config: %s", strerror(err));
		goto pipe_error;
	}

	/* now try and connect to the sof-pipe for this topology */
	err = plug_init_sof_pipe(plug, pcmp, name, stream, mode);
	if (err < 0) {
		SNDERR("failed to complete plugin init: %s", strerror(err));
		goto pipe_error;
	}

	/* everything is good */
	return 0;

pipe_error:
	free(plug->device);
dev_error:
	free(plug);
	return err;
}

SND_PCM_PLUGIN_SYMBOL(sof);
