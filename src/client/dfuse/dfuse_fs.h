/**
 * (C) Copyright 2017-2019 Intel Corporation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * GOVERNMENT LICENSE RIGHTS-OPEN SOURCE SOFTWARE
 * The Government's rights to use, modify, reproduce, release, perform, display,
 * or disclose this software are subject to the terms of the Apache License as
 * provided in Contract No. B609815.
 * Any reproduction of computer software, computer software documentation, or
 * portions thereof marked with this legend must also reproduce the markings.
 */
#ifndef __IOF_FS_H__
#define __IOF_FS_H__

#include <stdbool.h>
#include <sched.h>
#include <cart/types.h>
#include <gurt/atomic.h>
#include "dfuse_gah.h"

struct dfuse_service_group {
	crt_group_t		*dest_grp; /* Server group */
	crt_endpoint_t		psr_ep;    /* Server PSR endpoint */
	bool			enabled;   /* Indicates group is available */
};

/** Projection specific information held on the client.
 *
 * Shared between CNSS and IL.
 */
struct dfuse_projection {
	/** Server group info */
	struct dfuse_service_group	*grp;
	/** Protocol used for I/O RPCs */
	struct crt_proto_format		*io_proto;
	/** context to use */
	crt_context_t			crt_ctx;
	/** bulk threshold */
	uint32_t			max_iov_write;
	/** max write size */
	uint32_t			max_write;
	/** client projection id */
	int				cli_fs_id;
	/** Projection enabled flag */
	bool				enabled;
	/** True if there is a progress thread configured */
	bool				progress_thread;
};

/* Common data stored on open file handles */
struct dfuse_file_common {
	struct dfuse_projection	*projection;
	struct ios_gah		gah;
	crt_endpoint_t		ep;
};

/* Tracks remaining events for completion */
struct dfuse_tracker {
	ATOMIC int remaining;
};

/* Initialize number of events to track */
static inline void dfuse_tracker_init(struct dfuse_tracker *tracker,
				    int expected_count)
{
	atomic_store_release(&tracker->remaining, expected_count);
}

/* Signal an event */
static inline void dfuse_tracker_signal(struct dfuse_tracker *tracker)
{
	atomic_dec_release(&tracker->remaining);
}

/* Test if all events have signaled */
static inline bool dfuse_tracker_test(struct dfuse_tracker *tracker)
{
	if (atomic_load_consume(&tracker->remaining) == 0)
		return true;

	return false;
}

static inline void dfuse_tracker_wait(struct dfuse_tracker *tracker)
{
	while (!dfuse_tracker_test(tracker))
		sched_yield();
}

/* Progress until all events have signaled */
void dfuse_wait(crt_context_t, struct dfuse_tracker *);

/* Progress until all events have signaled */
static inline void dfuse_fs_wait(struct dfuse_projection *dfuse_state,
			       struct dfuse_tracker *tracker)
{
	/* If there is no progress thread then call progress from within
	 * this function, else just wait
	 */
	if (!dfuse_state->progress_thread) {
		dfuse_wait(dfuse_state->crt_ctx, tracker);
		return;
	}

	dfuse_tracker_wait(tracker);
}

#endif