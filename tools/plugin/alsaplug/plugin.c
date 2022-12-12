/*-*- linux-c -*-*/

/*
 * ALSA <-> SOF PCM I/O plugin
 *
 * Copyright(c) 2022 Intel Corporation. All rights reserved.
 *
 * This library is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

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

#include "plugin.h"
#include "common.h"

int plug_mq_cmd(struct plug_mq_desc *ipc, void *msg, size_t len, void *reply, size_t rlen)
{
	struct timespec ts;
	ssize_t ipc_size;
	char mailbox[IPC3_MAX_MSG_SIZE];
	int err;

	if (len > IPC3_MAX_MSG_SIZE) {
		SNDERR("ipc: message too big %d\n", len);
		return -EINVAL;
	}
	memset(mailbox, 0, IPC3_MAX_MSG_SIZE);
	memcpy(mailbox, msg, len);

	/* wait for sof-pipe reader to consume data or timeout */
	err = clock_gettime(CLOCK_REALTIME, &ts);
	if (err == -1) {
		SNDERR("ipc: cant get time: %s", strerror(errno));
		return -errno;
	}

	/* IPCs should be read under 10ms */
	plug_timespec_add_ms(&ts, 10);

	/* now return message completion status */
	err = mq_timedsend(ipc->mq, mailbox, IPC3_MAX_MSG_SIZE, 0, &ts);
	if (err < 0) {
		SNDERR("error: can't send IPC message queue %s : %s\n",
			ipc->queue_name, strerror(errno));
		return -errno;
	}

	/* wait for sof-pipe reader to consume data or timeout */
	err = clock_gettime(CLOCK_REALTIME, &ts);
	if (err == -1) {
		SNDERR("ipc: cant get time: %s", strerror(errno));
		return -errno;
	}

	/* IPCs should be processed under 20ms */
	plug_timespec_add_ms(&ts, 20);

	ipc_size = mq_timedreceive(ipc->mq, mailbox, IPC3_MAX_MSG_SIZE, NULL, &ts);
	if (ipc_size < 0) {
		SNDERR("error: can't read IPC message queue %s : %s\n",
			ipc->queue_name, strerror(errno));
		return -errno;
	}

	/* do the message work */
	if (rlen && reply)
		memcpy(reply, mailbox, rlen);

	return 0;
}

/*
 * Open an existing message queue using IPC object.
 */
int plug_mq_open(struct plug_mq_desc *ipc)
{
	/* now open new queue for Tx and Rx */
	ipc->mq = mq_open(ipc->queue_name,  O_RDWR);
	if (ipc->mq < 0) {
	//	SNDERR("failed to open IPC queue %s: %s\n",
	//		ipc->queue_name, strerror(errno));
		return -errno;
	}

	return 0;
}

/*
 * Open an existing semaphore using lock object.
 */
int plug_lock_open(struct plug_sem_desc *lock)
{
	lock->sem = sem_open(lock->name, O_RDWR);
	if (lock->sem == SEM_FAILED) {
		SNDERR("failed to open semaphore %s: %s\n",
				lock->name, strerror(errno));
		return -errno;
	}

	return 0;
}

/*
 * Open an existing shared memory region using the SHM object.
 */
int plug_shm_open(struct plug_shm_desc *shm)
{
	/* open SHM to be used for low latency position */
	shm->fd = shm_open(shm->name, O_RDWR,
			   S_IRWXU | S_IRWXG);
	if (shm->fd < 0) {
		//SNDERR("failed to open SHM position %s: %s\n",
		//	shm->name, strerror(errno));
		return -errno;
	}

	/* map it locally for context readback */
	shm->addr = mmap(NULL, shm->size,
				  PROT_READ | PROT_WRITE,
				  MAP_SHARED, shm->fd, 0);
	if (shm->addr == NULL) {
		SNDERR("failed to mmap SHM position%s: %s\n",
			shm->name, strerror(errno));
		return -errno;
	}

	return 0;
}

/*
 * Parse the ALSA conf for the SOF plugin and construct the command line options
 * to be passed into the SOF pipe executable.
 * TODO: verify all args
 * TODO: validate all arge.
 * TODO: contruct sof pipe cmd line.
 */
int plug_parse_conf(snd_sof_plug_t *plug, const char *name, snd_config_t *root,
		    snd_config_t *conf)
{
	snd_config_iterator_t i, next;
	const char *tplg_file = NULL;
	long tplg_pcm = 0;

	/*
	 * The topology filename and topology PCM need to be passed in.
	 * i.e. aplay -Dsof:file,plug
	 */
	snd_config_for_each(i, next, conf) {
		snd_config_t *n = snd_config_iterator_entry(i);
		const char *id;
		if (snd_config_get_id(n, &id) < 0)
			continue;

		/* dont care */
		if (strcmp(id, "comment") == 0 || strcmp(id, "type") == 0
		    || strcmp(id, "hint") == 0)
			continue;

		/* topology file name */
		if (strcmp(id, "tplg_file") == 0) {
			if (snd_config_get_string(n, &tplg_file) < 0) {
				SNDERR("Invalid type for %s", id);
				return -EINVAL;
			} else if (!*tplg_file) {
				tplg_file = NULL;
			}
			continue;
		}

		/* PCM ID in the topology file */
		if (strcmp(id, "tplg_pcm") == 0) {
			if (snd_config_get_integer(n, &tplg_pcm) < 0) {
				SNDERR("Invalid type for %s", id);
				return -EINVAL;
			}
			continue;
		}

		/* not fatal - carry on and verify later */
		SNDERR("Unknown field %s", id);
	}

	/* verify mandatory inputs are specified */
	if (!tplg_file) {
		SNDERR("Missing topology file");
		return -EINVAL;
	}
	plug->tplg_file = strdup(tplg_file);
	plug->tplg_pcm = tplg_pcm;

	printf("plug: topology file %s pipe %ld\n", tplg_file, tplg_pcm);

	plug->device = strdup(tplg_file);
	if (!plug->device) {
		return -ENOMEM;
	}

	return 0;
}
