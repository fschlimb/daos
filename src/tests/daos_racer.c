/**
 * (C) Copyright 2019 Intel Corporation.
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
#define D_LOGFAC       DD_FAC(tests)

#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <getopt.h>
#include <mpi.h>
#include <daos/common.h>
#include <daos/tests_lib.h>
#include <daos_test.h>
#include "dts_common.h"

enum {
	UPDATE,
	FETCH,
	ENUM_DKEY,
	ENUM_AKEY,
	ENUM_REC,
	PUNCH_DKEY,
	PUNCH_AKEY,
	PUNCH_REC,
	PUNCH_OBJ,
	TEST_SIZE,
};

#define MAX_ROUND	10
#define MAX_REC_SIZE	(4 * 1024)	/* MAX REC */
#define MAX_KEY_SIZE	32
#define MAX_KEY_CNT	10

enum {
	RP_XSF,
	RP_2G1,
	RP_2G2,
	RP_3G1,
	RP_3G2,
	OBJ_CNT
};

static struct dts_context	ts_ctx;
unsigned		seed;
int			dkey_cnt = MAX_KEY_CNT;
int			akey_cnt = MAX_KEY_CNT;
int			max_akey_per_dkey = 5;
int			obj_cnt_per_class = 2;

static uint16_t
oclass_get(unsigned int random)
{
	uint16_t idx = random % OBJ_CNT;

	switch (idx) {
	case RP_XSF:
		return OC_RP_XSF;
	case RP_2G1:
		return OC_RP_2G1;
	case RP_2G2:
		return OC_RP_2G2;
	case RP_3G1:
		return OC_RP_3G1;
	case RP_3G2:
		return OC_RP_3G2;
	default:
		assert(0);
	}

	return -1;
}

daos_obj_id_t
racer_oid_gen(int random)
{
	daos_obj_id_t	oid;
	uint64_t	hdr;
	uint16_t	oclass;

	oclass = oclass_get(random);

	hdr = oclass;
	hdr <<= 32;

	oid.lo	= random % obj_cnt_per_class;
	oid.lo	|= oclass;
	oid.hi	= oclass;
	daos_obj_generate_id(&oid, 0, oclass, 0);

	return oid;
}

void
pack_dkey_iod_sgl(char *dkey, d_iov_t *dkey_iov, char akeys[][MAX_KEY_SIZE],
		  daos_iod_t *iods, daos_recx_t *recxs, d_sg_list_t *sgls,
		  d_iov_t *iovs, char sgl_bufs[][MAX_REC_SIZE], int iod_nr)
{
	int i;

	sprintf(dkey, "%d", rand() % dkey_cnt);
	d_iov_set(dkey_iov, dkey, strlen(dkey));

	for (i = 0; i < iod_nr; i++) {
		unsigned size;

		sprintf(akeys[i], "%d", rand() % max_akey_per_dkey);
		d_iov_set(&iods[i].iod_name, akeys[i], strlen(akeys[i]));

		iods[i].iod_nr = 1;
		if (rand() % 2 == 1) {
			recxs[i].rx_idx = rand() % MAX_REC_SIZE;
			recxs[i].rx_nr = rand() % MAX_REC_SIZE;
			iods[i].iod_recxs = &recxs[i];
			iods[i].iod_size = 1;
			size = recxs[i].rx_nr;
			iods[i].iod_type = DAOS_IOD_ARRAY;
		} else {
			iods[i].iod_size = rand() % MAX_REC_SIZE;
			size = iods[i].iod_size;
			iods[i].iod_type = DAOS_IOD_SINGLE;
		}

		sgls[i].sg_nr = 1;
		sgls[i].sg_nr_out = 1;
		d_iov_set(&iovs[i], sgl_bufs[i], size);
		sgls[i].sg_iovs = &iovs[i];
	}
}

static void
update_or_fetch(bool update)
{
	daos_obj_id_t	ts_oid;		/* object ID */
	char		dkey[MAX_KEY_SIZE];
	char		akeys[max_akey_per_dkey][MAX_KEY_SIZE];
	daos_iod_t	iods[max_akey_per_dkey];
	d_sg_list_t	sgls[max_akey_per_dkey];
	char		sgl_bufs[max_akey_per_dkey][MAX_REC_SIZE];
	daos_recx_t	recxs[max_akey_per_dkey];
	d_iov_t		sgl_iovs[max_akey_per_dkey];
	d_iov_t		dkey_iov;
	int		random = rand();
	int		round = random % MAX_ROUND;
	daos_handle_t	oh;
	int		i;
	int		rc;

	ts_oid = racer_oid_gen(random);
	rc = daos_obj_open(ts_ctx.tsc_coh, ts_oid, DAOS_OO_RW, &oh, NULL);
	if (rc)
		return;

	for (i = 0; i < round; i++) {
		int iod_nr = random % max_akey_per_dkey;

		memset(iods, 0, max_akey_per_dkey * sizeof(daos_iod_t));
		pack_dkey_iod_sgl(dkey, &dkey_iov, akeys, iods, recxs, sgls,
				  sgl_iovs, sgl_bufs, iod_nr);
		if (update)
			daos_obj_update(oh, DAOS_TX_NONE, &dkey_iov, iod_nr,
					iods, sgls, NULL);
		else
			daos_obj_fetch(oh, DAOS_TX_NONE, &dkey_iov, iod_nr,
				       iods, sgls, NULL, NULL);
	}

	daos_obj_close(oh, NULL);
}

void
update(void)
{
	update_or_fetch(true);
}

void
fetch(void)
{
	update_or_fetch(false);
}

#define ENUM_SIZE	10
void
enum_internal(int op)
{
	daos_obj_id_t	ts_oid;		/* object ID */
	d_iov_t		dkey_iov;
	char		dkey[MAX_KEY_SIZE];
	d_iov_t		akey_iov;
	char		akey[MAX_KEY_SIZE];
	d_sg_list_t	sgl;
	char		sgl_bufs[4096];
	d_iov_t		sgl_iov;
	daos_anchor_t	anchor = { 0 };
	daos_handle_t	oh;
	int		rc;

	ts_oid = racer_oid_gen(rand());
	rc = daos_obj_open(ts_ctx.tsc_coh, ts_oid, DAOS_OO_RW, &oh, NULL);
	if (rc)
		return;

	d_iov_set(&sgl_iov, sgl_bufs, 4096);
	sgl.sg_iovs = &sgl_iov;
	sgl.sg_nr_out = sgl.sg_nr = 1;
	while (!daos_anchor_is_eof(&anchor)) {
		uint32_t nr = ENUM_SIZE;
		daos_key_desc_t kds[ENUM_SIZE];

		if (op == ENUM_DKEY) {
			rc = daos_obj_list_dkey(oh, DAOS_TX_NONE, &nr, kds,
						&sgl, &anchor, NULL);
		} else {
			daos_recx_t		recxs[ENUM_SIZE];
			daos_epoch_range_t	eprs[ENUM_SIZE];

			sprintf(dkey, "%d", rand() % dkey_cnt);
			d_iov_set(&dkey_iov, dkey, strlen(dkey));
			if (op == ENUM_AKEY) {
				rc = daos_obj_list_akey(oh, DAOS_TX_NONE,
						&dkey_iov, &nr, kds, &sgl,
						&anchor, NULL);
			} else {
				daos_size_t size;

				sprintf(akey, "%d", rand() % max_akey_per_dkey);
				d_iov_set(&akey_iov, akey, strlen(akey));
				rc = daos_obj_list_recx(oh, DAOS_TX_NONE,
						&dkey_iov, &akey_iov, &size,
						&nr, recxs, eprs, &anchor,
						true, NULL);
			}
		}
		if (rc)
			break;
	}

	daos_obj_close(oh, NULL);
}

void
enum_dkey(void)
{
	enum_internal(ENUM_DKEY);
}

void
enum_akey(void)
{
	enum_internal(ENUM_AKEY);
}

void
enum_rec(void)
{
	enum_internal(ENUM_REC);
}

void
punch_internal(int op)
{
	daos_obj_id_t	ts_oid;		/* object ID */
	d_iov_t		dkey_iov;
	char		dkey[MAX_KEY_SIZE];
	d_iov_t		akey_iov;
	char		akey[MAX_KEY_SIZE];
	daos_handle_t	oh;
	int		rc;

	ts_oid = racer_oid_gen(rand());
	rc = daos_obj_open(ts_ctx.tsc_coh, ts_oid, DAOS_OO_RW, &oh, NULL);
	if (rc)
		return;

	if (op == PUNCH_OBJ) {
		daos_obj_punch(oh, DAOS_TX_NONE, NULL);
	} else {
		sprintf(dkey, "%d", rand() % dkey_cnt);
		d_iov_set(&dkey_iov, dkey, strlen(dkey));
		if (op == PUNCH_DKEY) {
			daos_obj_punch_dkeys(oh, DAOS_TX_NONE, 1, &dkey_iov,
					     NULL);
		} else {
			sprintf(akey, "%d", rand() % max_akey_per_dkey);
			d_iov_set(&akey_iov, akey, strlen(akey));
			daos_obj_punch_akeys(oh, DAOS_TX_NONE, &dkey_iov, 1,
					     &akey_iov, NULL);
		}
	}
	daos_obj_close(oh, NULL);
}

void
punch_obj(void)
{
	punch_internal(PUNCH_OBJ);
}

void
punch_dkey(void)
{
	punch_internal(PUNCH_DKEY);
}

void
punch_akey(void)
{
	punch_internal(PUNCH_AKEY);
}

struct racer_sub_tests {
	void (*sub_test)(void);
};

struct racer_sub_tests racer_tests[] = {
	[UPDATE] = {.sub_test = update},
	[FETCH] = {.sub_test = fetch},
	[ENUM_DKEY] = {.sub_test = enum_dkey},
	[ENUM_AKEY] = {.sub_test = enum_akey},
	[ENUM_REC] = {.sub_test = enum_rec},
	[PUNCH_OBJ] = {.sub_test = punch_obj},
	[PUNCH_DKEY] = {.sub_test = punch_dkey},
	[PUNCH_AKEY] = {.sub_test = punch_akey},
};

void sub_tests_init(struct racer_sub_tests *tests, uint32_t bits)
{
	int i;
	uint32_t mask;

	for (i = 0; i < TEST_SIZE; i++) {
		mask = 1 << i;
		if (mask & bits)
			tests[i].sub_test = racer_tests[i].sub_test;
	}
}

static int
racer_test_idx(struct racer_sub_tests *tests)
{
	int idx;

	idx = ts_ctx.tsc_mpi_rank % TEST_SIZE;
	while (tests[idx].sub_test == NULL)
		idx = (idx + 1) % TEST_SIZE;

	return idx;
}

static struct option ts_ops[] = {
	{ "pool_uuid",	required_argument,	NULL,	'p' },
	{ "cont_uuid",	required_argument,	NULL,	'c' },
	{ "time",	required_argument,	NULL,	't' },
	{ NULL,		0,			NULL,	0   },
};

int
main(int argc, char **argv)
{
	struct timeval	tv;
	daos_size_t	scm_size = (2ULL << 30); /* default pool SCM size */
	daos_size_t	nvme_size = (8ULL << 30); /* default pool NVMe size */
	d_rank_t	svc_rank  = 0;	/* pool service rank */
	unsigned	duration = 60; /* seconds */
	double		expire = 0;
	int		idx;
	struct racer_sub_tests	sub_tests[TEST_SIZE] = { 0 };
	int		rc;

	MPI_Init(&argc, &argv);
	MPI_Comm_rank(MPI_COMM_WORLD, &ts_ctx.tsc_mpi_rank);
	MPI_Comm_size(MPI_COMM_WORLD, &ts_ctx.tsc_mpi_size);
	while ((rc = getopt_long(argc, argv,
				 "p:c:t:",
				 ts_ops, NULL)) != -1) {
		char	*endp;

		switch (rc) {
		default:
			fprintf(stderr, "Unknown option %c\n", rc);
			return -1;
		case 'p':
			rc = uuid_parse(optarg, ts_ctx.tsc_pool_uuid);
			if (rc)
				return rc;
			break;
		case 'c':
			rc = uuid_parse(optarg, ts_ctx.tsc_cont_uuid);
			if (rc)
				return rc;
		case 't':
			duration = strtoul(optarg, &endp, 0);
			break;
		}
	}

	if (seed == 0) {
		gettimeofday(&tv, NULL);
		seed = tv.tv_usec;
	}
	srand(seed);

	if (ts_ctx.tsc_mpi_rank == 0 && uuid_is_null(ts_ctx.tsc_pool_uuid))
		uuid_generate(ts_ctx.tsc_pool_uuid);
	if (ts_ctx.tsc_mpi_rank == 0 && uuid_is_null(ts_ctx.tsc_cont_uuid))
		uuid_generate(ts_ctx.tsc_cont_uuid);

	ts_ctx.tsc_svc.rl_nr = 1;
	ts_ctx.tsc_svc.rl_ranks  = &svc_rank;
	ts_ctx.tsc_scm_size	= scm_size;
	ts_ctx.tsc_nvme_size	= nvme_size;

	if (ts_ctx.tsc_mpi_rank == 0) {
		fprintf(stdout,
			"racer start with %d threads duration %u secs\n"
			"\tpool size     : SCM: %u MB, NVMe: %u MB\n",
			ts_ctx.tsc_mpi_size, duration,
			(unsigned int)(scm_size >> 20),
			(unsigned int)(nvme_size >> 20));
	}

	rc = dts_ctx_init(&ts_ctx);
	if (rc)
		D_GOTO(out, rc);

	sub_tests_init(sub_tests, 0xFFFF);
	expire = dts_time_now() + duration;

	idx = racer_test_idx(sub_tests);
	MPI_Barrier(MPI_COMM_WORLD);
	while (1) {
		sub_tests[idx].sub_test();
		if (dts_time_now() > expire)
			break;
	}
	MPI_Barrier(MPI_COMM_WORLD);

	dts_ctx_fini(&ts_ctx);
out:
	MPI_Finalize();
	return rc;
}
