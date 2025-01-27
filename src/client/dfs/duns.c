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
/**
 * DAOS Unified namespace functionality.
 */

#define D_LOGFAC	DD_FAC(duns)

#include <dirent.h>
#include <libgen.h>
#include <sys/xattr.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <dlfcn.h>
#ifdef LUSTRE_INCLUDE
#include <lustre/lustreapi.h>
#include <linux/lustre/lustre_idl.h>
#else
#include "daos_uns_lustre.h"
#endif
#include <daos/common.h>
#include <daos/object.h>
#include "daos_types.h"
#include "daos.h"
#include "daos_fs.h"
#include "daos_uns.h"

#define DUNS_XATTR_NAME		"user.daos"
#define DUNS_MAX_XATTR_LEN	170
#define DUNS_MIN_XATTR_LEN	90
#define DUNS_XATTR_FMT		"DAOS.%s://%36s/%36s/%s/%zu"
#define LIBLUSTRE		"liblustreapi.so"

/* XXX may need to use ioctl() direct method instead of Lustre
 * API if moving from Lustre build/install dependency to pure
 * dynamic/run-time discovery+binding of/with liblustreapi.so
 */

static bool liblustre_notfound = false;
/* need to protect against concurent/multi-threaded attempts to bind ? */
static bool liblustre_binded = false;
static int (*dir_create_foreign)(const char *, mode_t, __u32, __u32,
				 const char *) = NULL;
int (*unlink_foreign)(char *) = NULL;

static int
bind_liblustre()
{
	void *lib;

	/* bind to lustre lib/API */
	lib = dlopen(LIBLUSTRE, RTLD_NOW);
	if (lib == NULL) {
		liblustre_notfound = true;
		D_ERROR("unable to locate/bind %s, dlerror() says '%s', "
			"reverting to non-lustre behaviour.\n",
			LIBLUSTRE, dlerror());
		return -DER_INVAL;
	}

	D_DEBUG(DB_TRACE, "%s has been found and dynamicaly binded !\n",
		LIBLUSTRE);

	/* now try to map the API methods we need */
	dir_create_foreign = (int (*)(const char *, mode_t, __u32, __u32,
			      const char *))dlsym(lib,
						  "llapi_dir_create_foreign");
	if (dir_create_foreign == NULL) {
		liblustre_notfound = true;
		D_ERROR("unable to resolve llapi_dir_create_foreign symbol, "
			"dlerror() says '%s', Lustre version do not seem to "
			"support foreign LOV/LMV, reverting to non-lustre "
			"behaviour.\n", dlerror());
		return -DER_INVAL;
	}

	D_DEBUG(DB_TRACE, "llapi_dir_create_foreign() resolved at %p\n",
		dir_create_foreign);

	unlink_foreign = (int (*)(char *))dlsym(lib,
						      "llapi_unlink_foreign");
	if (unlink_foreign == NULL) {
		liblustre_notfound = true;
		D_ERROR("unable to resolve llapi_unlink_foreign symbol, "
			"dlerror() says '%s', Lustre version do not seem to "
			"support foreign daos type, reverting to non-lustre "
			"behaviour.\n", dlerror());
		return -DER_INVAL;
	}

	D_DEBUG(DB_TRACE, "llapi_unlink_foreign() resolved at %p\n",
		unlink_foreign);

	liblustre_binded = true;
	return 0;
}

static int
duns_resolve_lustre_path(const char *path, struct duns_attr_t *attr)
{
	char			str[DUNS_MAX_XATTR_LEN + 1];
	char			*saveptr, *t;
	char			*buf;
	struct lmv_user_md	*lum;
	struct lmv_foreign_md	*lfm;
	int			fd;
	int			rc;

	/* XXX if a Posix container is not always mapped with a daos foreign dir
	 * with LMV, both LOV and LMV will need to be queried if ENODATA is
	 * returned at 1st, as the file/dir type is hidden to help decide before
	 * due to the symlink fake !!
	 * Also, querying/checking container's type/oclass/chunk_size/...
	 * vs EA content could be a good idea ?
	 */

	/* XXX if liblustreapi is not binded, do it now ! */
	if (liblustre_binded == false && liblustre_notfound == false) {
		rc = bind_liblustre();
		if (rc)
			return -DER_INVAL;
	} else if (liblustre_notfound == true) {
		/* no liblustreapi.so found, or incompatible */
		return -DER_INVAL;
	} else if (liblustre_binded == false) {
		/* this should not happen */
		D_ERROR("liblustre_notfound == false && liblustre_notfound == "
			"false not expected after bind_liblustre()\n");
		return -DER_INVAL;
	}

	D_DEBUG(DB_TRACE, "Trying to retrieve associated container's infos "
		"from Lustre path '%s'\n", path);

	/* get LOV/LMV
	 * llapi_getstripe() API can not be used here as it frees
	 * param.[fp_lmv_md, fp_lmd->lmd_lmm]  buffer for LMV after printing
	 * its content
	 * raw/ioctl() way must be used then
	 */
	buf = calloc(1, XATTR_SIZE_MAX);
	if (buf == NULL) {
		D_ERROR("unable to allocate XATTR_SIZE_MAX to get LOV/LMV "
			"for '%s', errno %d(%s).\n", path, errno,
			strerror(errno));
		/** TODO - convert errno to rc */
		return -DER_NOSPACE;
	}
	fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	if (fd == -1 && errno != ENOTDIR) {
		D_ERROR("unable to open '%s' errno %d(%s).\n", path, errno,
			strerror(errno));
		/** TODO - convert errno to rc */
		return -DER_INVAL;
	} else if (errno == ENOTDIR) {
		/* should we handle file/LOV case ?
		 * for link to HDF5 container ?
		 */
		D_ERROR("file with foreign LOV support is presently not "
			"supported\n");
	} else {
		/* it is a dir so get LMV ! */
		lum = (struct lmv_user_md *)buf;
		/* to get LMV and not default one !! */
		lum->lum_magic = LMV_MAGIC_V1;
		/* to confirm we have already a buffer large enough get a
		 * BIG LMV !!
		 */
		lum->lum_stripe_count = (XATTR_SIZE_MAX - 
					sizeof(struct lmv_user_md)) /
					sizeof(struct lmv_user_mds_data);
		rc = ioctl(fd, LL_IOC_LMV_GETSTRIPE, buf);
		if (rc != 0) {
			D_ERROR("ioctl(LL_IOC_LMV_GETSTRIPE) failed, rc: %d, "
				"errno %d(%s).\n", rc, errno, strerror(errno));
			/** TODO - convert errno to rc */
			return -DER_INVAL;
		}
	
		lfm = (struct lmv_foreign_md *)buf;
		/* sanity check */
		if (lfm->lfm_magic != LMV_MAGIC_FOREIGN  ||
		    lfm->lfm_type != LU_FOREIGN_TYPE_DAOS ||
		    lfm->lfm_length > DUNS_MAX_XATTR_LEN ||
		    snprintf(str, DUNS_MAX_XATTR_LEN, "%s",
			     lfm->lfm_value) > DUNS_MAX_XATTR_LEN) {
			D_ERROR("Invalid DAOS LMV format (%s).\n", str);
			return -DER_INVAL;
		}
	}

	t = strtok_r(str, ".", &saveptr);
	if (t == NULL) {
		D_ERROR("Invalid DAOS LMV format (%s).\n", str);
		return -DER_INVAL;
	}

	t = strtok_r(NULL, ":", &saveptr);
	if (t == NULL) {
		D_ERROR("Invalid DAOS LMV format (%s).\n", str);
		return -DER_INVAL;
	}

	daos_parse_ctype(t, &attr->da_type);
	if (attr->da_type == DAOS_PROP_CO_LAYOUT_UNKOWN) {
		D_ERROR("Invalid DAOS LMV format: Container layout cannot be"
			" unknown\n");
		return -DER_INVAL;
	}

	t = strtok_r(NULL, "/", &saveptr);
	if (t == NULL) {
		D_ERROR("Invalid DAOS LMV format (%s).\n", str);
		return -DER_INVAL;
	}

	rc = uuid_parse(t, attr->da_puuid);
	if (rc) {
		D_ERROR("Invalid DAOS LMV format: pool UUID cannot be"
			" parsed\n");
		return -DER_INVAL;
	}

	t = strtok_r(NULL, "/", &saveptr);
	if (t == NULL) {
		D_ERROR("Invalid DAOS LMV format (%s).\n", str);
		return -DER_INVAL;
	}

	rc = uuid_parse(t, attr->da_cuuid);
	if (rc) {
		D_ERROR("Invalid DAOS LMV format: container UUID cannot be"
			" parsed\n");
		return -DER_INVAL;
	}

	t = strtok_r(NULL, "/", &saveptr);
	if (t == NULL) {
		D_ERROR("Invalid DAOS LMV format (%s).\n", str);
		return -DER_INVAL;
	}

	/* path is DAOS-foreign and will need to be unlinked using
	 * unlink_foreign API
	 */
	attr->da_on_lustre = true;

	attr->da_oclass_id = daos_oclass_name2id(t);

	t = strtok_r(NULL, "/", &saveptr);
	attr->da_chunk_size = strtoull(t, NULL, 10);

	return 0;
}

int
duns_resolve_path(const char *path, struct duns_attr_t *attr)
{
	ssize_t	s;
	char	str[DUNS_MAX_XATTR_LEN];
	char	*saveptr, *t;
	struct statfs fs;
	char *dir, *dirp;
	int	rc;

	dir = malloc(PATH_MAX);
	if (dir == NULL) {
		D_ERROR("Failed to allocate %d bytes for required copy of "
			"path %s: %s\n", PATH_MAX, path, strerror(errno));
		/** TODO - convert errno to rc */
		return -DER_NOSPACE;
	}

	dirp = strcpy(dir, path);
	/* dirname() may modify dir content or not, so use an
	 * alternate pointer (see dirname() man page)
	 */
	dirp = dirname(dir);
	rc = statfs(dirp, &fs);
	if (rc == -1) {
		D_ERROR("Failed to statfs %s: %s\n", path, strerror(errno));
		/** TODO - convert errno to rc */
		return -DER_INVAL;
	}

	if (fs.f_type == LL_SUPER_MAGIC) {
		rc = duns_resolve_lustre_path(path, attr);
		if (rc == 0)
			return 0;
		/* if Lustre specific method fails, fallback to try
		 * the normal way...
		 */
	}

	s = lgetxattr(path, DUNS_XATTR_NAME, &str, DUNS_MAX_XATTR_LEN);
	if (s < 0 || s > DUNS_MAX_XATTR_LEN) {
		if (s == ENOTSUP)
			D_ERROR("Path is not in a filesystem that supports the"
				" DAOS unified namespace\n");
		else if (s == ENODATA)
			D_ERROR("Path does not represent a DAOS link\n");
		else if (s > DUNS_MAX_XATTR_LEN)
			D_ERROR("Invalid xattr length\n");
		else
			D_ERROR("Invalid DAOS unified namespace xattr\n");
		return -DER_INVAL;
	}

	t = strtok_r(str, ".", &saveptr);
	if (t == NULL) {
		D_ERROR("Invalid DAOS xattr format (%s).\n", str);
		return -DER_INVAL;
	}

	t = strtok_r(NULL, ":", &saveptr);
	daos_parse_ctype(t, &attr->da_type);
	if (attr->da_type == DAOS_PROP_CO_LAYOUT_UNKOWN) {
		D_ERROR("Invalid DAOS xattr format: Container layout cannot be"
			" unknown\n");
		return -DER_INVAL;
	}

	t = strtok_r(NULL, "/", &saveptr);
	rc = uuid_parse(t, attr->da_puuid);
	if (rc) {
		D_ERROR("Invalid DAOS xattr format: pool UUID cannot be"
			" parsed\n");
		return -DER_INVAL;
	}

	t = strtok_r(NULL, "/", &saveptr);
	rc = uuid_parse(t, attr->da_cuuid);
	if (rc) {
		D_ERROR("Invalid DAOS xattr format: container UUID cannot be"
			" parsed\n");
		return -DER_INVAL;
	}

	t = strtok_r(NULL, "/", &saveptr);
	attr->da_oclass_id = daos_oclass_name2id(t);

	t = strtok_r(NULL, "/", &saveptr);
	attr->da_chunk_size = strtoull(t, NULL, 10);

	return 0;
}

static int
duns_create_lustre_path(daos_handle_t poh, const char *path,
			struct duns_attr_t *attrp)
{
	char			pool[37], cont[37];
	char			oclass[10], type[10];
	char			str[DUNS_MAX_XATTR_LEN + 1];
	int			len;
	int			try_multiple = 1;		/* boolean */
	int			rc;

	/* XXX pool must already be created, and associated DFuse-mount
	 * should already be mounted
	 */

	/* XXX if liblustreapi is not binded, do it now ! */
	if (liblustre_binded == false && liblustre_notfound == false) {
		rc = bind_liblustre();
		if (rc)
			return -DER_INVAL;
	}

	uuid_unparse(attrp->da_puuid, pool);
	daos_oclass_id2name(attrp->da_oclass_id, oclass);
	daos_unparse_ctype(attrp->da_type, type);

	/* create container with specified container uuid (try_multiple=0)
	 * or a generated random container uuid (try_multiple!=0).
	 */
	if (!uuid_is_null(attrp->da_cuuid)) {
		try_multiple = 0;
		uuid_unparse(attrp->da_cuuid, cont);
		D_INFO("try create once with provided container UUID: %36s\n",
			cont);
	}
	/* create container */
	do {
		if (try_multiple) {
			uuid_generate(attrp->da_cuuid);
			uuid_unparse(attrp->da_cuuid, cont);
		}

		if (attrp->da_type == DAOS_PROP_CO_LAYOUT_POSIX) {
			dfs_attr_t	dfs_attr;

			/** TODO: set Lustre FID here. */
			dfs_attr.da_id = 0;
			dfs_attr.da_oclass_id = attrp->da_oclass_id;
			dfs_attr.da_chunk_size = attrp->da_chunk_size;
			rc = dfs_cont_create(poh, attrp->da_cuuid, &dfs_attr,
					     NULL, NULL);
		} else {
			daos_prop_t	*prop;

			prop = daos_prop_alloc(1);
			if (prop == NULL) {
				D_ERROR("Failed to allocate container prop.");
				D_GOTO(err, rc = -DER_NOMEM);
			}
			prop->dpp_entries[0].dpe_type =
				DAOS_PROP_CO_LAYOUT_TYPE;
			prop->dpp_entries[0].dpe_val = attrp->da_type;
			rc = daos_cont_create(poh, attrp->da_cuuid, prop, NULL);
			daos_prop_free(prop);
		}

	} while ((rc == -DER_EXIST) && try_multiple);

	if (rc) {
		D_ERROR("Failed to create container (%d)\n", rc);
		D_GOTO(err, rc);
	}

	/* XXX should file with foreign LOV be expected/supoorted here ? */

	/** create dir and store the daos attributes in the path LMV */
	len = sprintf(str, DUNS_XATTR_FMT, type, pool, cont, oclass,
		      attrp->da_chunk_size);
	if (len < DUNS_MIN_XATTR_LEN) {
		D_ERROR("Failed to create LMV value\n");
		D_GOTO(err_cont, rc = -DER_INVAL);
	}

	rc = (*dir_create_foreign)(path, S_IRWXU | S_IRWXG | S_IROTH | S_IWOTH,
				   LU_FOREIGN_TYPE_DAOS, 0xda05, str);
	if (rc) {
		D_ERROR("Failed to create Lustre dir '%s' with foreign "
			"LMV '%s' (rc = %d).\n", path, str, rc);
		D_GOTO(err_cont, rc = -DER_INVAL);
	}

	return rc;

err_cont:
	daos_cont_destroy(poh, attrp->da_cuuid, 1, NULL);
err:
	return rc;
}

int
duns_create_path(daos_handle_t poh, const char *path, struct duns_attr_t *attrp)
{
	char			pool[37], cont[37];
	char			oclass[10], type[10];
	char			str[DUNS_MAX_XATTR_LEN];
	int			len;
	int			try_multiple = 1;		/* boolean */
	int			rc;

	if (path == NULL) {
		D_ERROR("Invalid path\n");
		return -DER_INVAL;
	}

	if (attrp->da_type == DAOS_PROP_CO_LAYOUT_HDF5) {
		/** create a new file if HDF5 container */
		int fd;

		fd = open(path, O_CREAT | O_EXCL,
			  S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH);
		if (fd == -1) {
			D_ERROR("Failed to create file %s: %s\n", path,
				strerror(errno));
			/** TODO - convert errno to rc */
			return -DER_INVAL;
		}
		close(fd);
	} else if (attrp->da_type == DAOS_PROP_CO_LAYOUT_POSIX) {
		struct statfs fs;
		char *dir, *dirp;

		dir = malloc(PATH_MAX);
		if (dir == NULL) {
			D_ERROR("Failed to allocate %d bytes for required "
				"copy of path %s: %s\n", PATH_MAX, path,
				strerror(errno));
			/** TODO - convert errno to rc */
			return -DER_NOSPACE;
		}

		dirp = strcpy(dir, path);
		/* dirname() may modify dir content or not, so use an
		 * alternate pointer (see dirname() man page)
		 */
		dirp = dirname(dir);
		rc = statfs(dirp, &fs);
		if (rc == -1) {
			D_ERROR("Failed to statfs dir %s: %s\n",
				dirp, strerror(errno));
			/** TODO - convert errno to rc */
			return -DER_INVAL;
		}

		if (fs.f_type == LL_SUPER_MAGIC) {
			rc = duns_create_lustre_path(poh, path, attrp);
			if (rc == 0)
				return 0;
			/* if Lustre specific method fails, fallback to try
			 * the normal way...
			 */
		}

		/** create a new directory if POSIX/MPI-IO container */
		rc = mkdir(path, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
		if (rc == -1) {
			D_ERROR("Failed to create dir %s: %s\n",
				path, strerror(errno));
			/** TODO - convert errno to rc */
			return -DER_INVAL;
		}
	} else {
		D_ERROR("Invalid container layout.\n");
		return -DER_INVAL;
	}

	uuid_unparse(attrp->da_puuid, pool);
	if (attrp->da_oclass_id != OC_UNKNOWN)
		daos_oclass_id2name(attrp->da_oclass_id, oclass);
	else
		strcpy(oclass, "UNKNOWN");
	daos_unparse_ctype(attrp->da_type, type);

	/* create container with specified container uuid (try_multiple=0)
	 * or a generated random container uuid (try_multiple!=0).
	 */
	if (!uuid_is_null(attrp->da_cuuid)) {
		try_multiple = 0;
		uuid_unparse(attrp->da_cuuid, cont);
		D_INFO("try create once with provided container UUID: %36s\n",
			cont);
	}
	do {
		if (try_multiple) {
			uuid_generate(attrp->da_cuuid);
			uuid_unparse(attrp->da_cuuid, cont);
		}

		/** store the daos attributes in the path xattr */
		len = sprintf(str, DUNS_XATTR_FMT, type, pool, cont, oclass,
			      attrp->da_chunk_size);
		if (len < DUNS_MIN_XATTR_LEN) {
			D_ERROR("Failed to create xattr value\n");
			D_GOTO(err_link, rc = -DER_INVAL);
		}

		rc = lsetxattr(path, DUNS_XATTR_NAME, str, len + 1, 0);
		if (rc) {
			D_ERROR("Failed to set DAOS xattr (rc = %d).\n", rc);
			D_GOTO(err_link, rc = -DER_INVAL);
		}

		if (attrp->da_type == DAOS_PROP_CO_LAYOUT_POSIX) {
			dfs_attr_t	dfs_attr;

			/** TODO: set Lustre FID here. */
			dfs_attr.da_id = 0;
			dfs_attr.da_oclass_id = attrp->da_oclass_id;
			dfs_attr.da_chunk_size = attrp->da_chunk_size;
			rc = dfs_cont_create(poh, attrp->da_cuuid, &dfs_attr,
					     NULL, NULL);
		} else {
			daos_prop_t	*prop;

			prop = daos_prop_alloc(1);
			if (prop == NULL) {
				D_ERROR("Failed to allocate container prop.");
				D_GOTO(err_link, rc = -DER_NOMEM);
			}
			prop->dpp_entries[0].dpe_type =
				DAOS_PROP_CO_LAYOUT_TYPE;
			prop->dpp_entries[0].dpe_val = attrp->da_type;
			rc = daos_cont_create(poh, attrp->da_cuuid, prop, NULL);
			daos_prop_free(prop);
		}
	} while ((rc == -DER_EXIST) && try_multiple);
	if (rc) {
		D_ERROR("Failed to create container (%d)\n", rc);
		D_GOTO(err_link, rc);
	}

	return rc;
err_link:
	if (attrp->da_type == DAOS_PROP_CO_LAYOUT_HDF5)
		unlink(path);
	else if (attrp->da_type == DAOS_PROP_CO_LAYOUT_POSIX)
		rmdir(path);
	return rc;
}

int
duns_destroy_path(daos_handle_t poh, const char *path)
{
	struct duns_attr_t dattr = {0};
	int	rc;

	/* Resolve pool, container UUIDs from path */
	rc = duns_resolve_path(path, &dattr);
	if (rc) {
		D_ERROR("duns_resolve_path() Failed on path %s (%d)\n",
			path, rc);
		return rc;
	}

	/** Destroy the container */
	rc = daos_cont_destroy(poh, dattr.da_cuuid, 1, NULL);
	if (rc) {
		D_ERROR("Failed to destroy container (%d)\n", rc);
		/** recreate the link ? */
		return rc;
	}

	if (dattr.da_type == DAOS_PROP_CO_LAYOUT_HDF5) {
		if (dattr.da_on_lustre)
			rc = (*unlink_foreign)((char *)path);
		else
			rc = unlink(path);
		if (rc) {
			D_ERROR("Failed to unlink %sfile %s: %s\n",
				dattr.da_on_lustre ? "Lustre " : " ", path,
				strerror(errno));
			return -DER_INVAL;
		}
	} else if (dattr.da_type == DAOS_PROP_CO_LAYOUT_POSIX) {
		if (dattr.da_on_lustre)
			rc = (*unlink_foreign)((char *)path);
		else
			rc = rmdir(path);
		if (rc) {
			D_ERROR("Failed to remove %sdir %s: %s\n",
				dattr.da_on_lustre ? "Lustre " : " ", path,
				strerror(errno));
			return -DER_INVAL;
		}
	}

	return 0;
}
