/* SPDX-License-Identifier: LGPL-2.1-or-later */

#pragma once


typedef struct PullJob PullJob;

typedef void (*PullJobFinished)(PullJob *job);
typedef int (*PullJobOpenDisk)(PullJob *job);
typedef int (*PullJobHeader)(PullJob *job, const char *header, size_t sz);
typedef void (*PullJobProgress)(PullJob *job);
typedef int (*PullJobNotFound)(PullJob *job, char **ret_new_url);

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

typedef struct {
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
        PullJobHeader on_header;
        PullJobProgress on_progress;
        PullJobNotFound on_not_found;

        char *etag;
        char **old_etags;
        bool etag_exists;

        uint64_t content_length;
        uint64_t written_compressed;
        uint64_t written_uncompressed;
        uint64_t offset;

        uint64_t uncompressed_max;
        uint64_t compressed_max;

        uint64_t expected_content_length;

        struct iovec payload;

        int disk_fd;
        bool close_disk_fd;
        struct stat disk_stat;

        usec_t mtime;

        unsigned progress_percent;
        usec_t start_usec;
        usec_t last_status_usec;

        bool calc_checksum;
        EVP_MD_CTX *checksum_ctx;

        struct iovec checksum;
        struct iovec expected_checksum;

        bool sync;
        bool force_memory;

        PullInstance *instances;
        size_t n_instances;
} PullJob;

int pull_job_new(PullJob **ret, const char *url, void *userdata);
PullJob* pull_job_unref(PullJob *job);

void pull_job_close_disk_fd(PullJob *j);

void pull_job_finish(PullJob *j, int ret);

DEFINE_TRIVIAL_CLEANUP_FUNC(PullJob*, pull_job_unref);

int pull_file_job_begin(PullJob *j);
