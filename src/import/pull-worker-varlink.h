/* SPDX-License-Identifier: LGPL-2.1-or-later */

#pragma once


typedef struct PullJob PullJob;
typedef struct PullInstance PullInstance;

typedef int (*PullJobFinished)(PullJob *job);
typedef int (*PullJobOpenDisk)(PullJob *job);
typedef void (*PullJobProgress)(PullJob *job);

#include "pull-common.h"

typedef enum PullJobState {
        PULL_JOB_INIT,
        PULL_JOB_ANALYZING, /* Still reading into ->payload, to figure out what we have */
        PULL_JOB_RUNNING,   /* Writing to destination */
        PULL_JOB_DONE,
        PULL_JOB_FAILED,
        _PULL_JOB_STATE_MAX,
        _PULL_JOB_STATE_INVALID = -EINVAL,
} PullJobState;

#define PULL_JOB_IS_COMPLETE(j) (IN_SET((j)->state, PULL_JOB_DONE, PULL_JOB_FAILED))

typedef struct PullInstance {
        uint64_t offset, size;
        int fd;
        char *path;
} PullInstance;

typedef struct PullJob {
        PullJobState state;
        int error;

        char *url;

        void *userdata;
        PullJobFinished on_finished;
        PullJobOpenDisk on_open_disk;
        PullJobProgress on_progress;

        char *etag;
        char **old_etags;
        bool etag_exists;

        uint64_t offset;

        uint64_t uncompressed_max;

        int disk_fd;
        struct stat disk_stat;

        usec_t mtime;

        unsigned progress_percent;

        struct iovec checksum;
        struct iovec expected_checksum;

        bool sync;

        PullInstance *instances;
        size_t n_instances;
} PullJob;

int pull_job_new(PullJob **ret, const char *url, void *userdata);
PullJob* pull_job_unref(PullJob *job);

void pull_job_close_disk_fd(PullJob *j);

int pull_job_finish(PullJob *j, int ret);

DEFINE_TRIVIAL_CLEANUP_FUNC(PullJob*, pull_job_unref);

int pull_file_job_begin(PullJob *j);
