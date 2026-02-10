/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "fd-util.h"
#include "fs-util.h"
#include "hexdecoct.h"
#include "io-util.h"
#include "log.h"
#include "path-util.h"
#include "import-util.h"
#include "import-common.h"
#include "pull-worker-varlink.h"
#include "sd-varlink.h"
#include "string-util.h"
#include "strv.h"
#include "varlink-util.h"

static int url_get_protocol(const char *url, const char **protocol) {
        const char *d;
        size_t length;

        assert(url);
        assert(protocol);

        /* Find colon separating protocol and hostname */
        d = strchr(url, ':');
        if (!d || url == d)
                return -EINVAL;

        length = d - url;

        *protocol = strndup(url, length);
        if (!*protocol)
                return -ENOMEM;
        return 0;
}

int pull_file_job_begin(PullJob *j) {
        int r;

        assert(j);

        if (j->state != PULL_JOB_INIT)
                return -EBUSY;

        const char *protocol;
        r = url_get_protocol(j->url, &protocol);
        if (r < 0)
                return log_error_errno(r, "Failed to parse protocol from URL %s: %m", j->url);

        _cleanup_(sd_varlink_flush_close_unrefp) sd_varlink *pull_link = NULL;
        r = sd_varlink_connect_address(&pull_link, path_join(SYSTEMD_PULL_WORKER_DIRECTORY_PATH, protocol));
        if (r < 0)
                return log_error_errno(r, "Failed to connect systemd-pull-job-varlink: %m");

        r = sd_varlink_set_allow_fd_passing_output(pull_link, true);
        if (r < 0)
                return log_debug_errno(r, "Failed to enable varlink fd passing for write: %m");


        j->on_open_disk(j);

        int destination_fd_index = sd_varlink_push_dup_fd(pull_link, j->disk_fd);
        if (destination_fd_index < 0)
                return log_error_errno(destination_fd_index, "Failed to push destination fd into varlink socket: %m");

        sd_json_variant *instances_array = NULL;
        FOREACH_ARRAY(instance, j->instances, j->n_instances) {
                int instance_fd_index = sd_varlink_push_fd(pull_link, TAKE_FD(instance->fd));
                if (instance_fd_index < 0)
                        return log_error_errno(instance_fd_index, "Failed to push instance fd into varlink socket: %m");

                r = sd_json_variant_append_arraybo(
                        &instances_array,
                        SD_JSON_BUILD_PAIR_UNSIGNED("locationFileDescriptor", instance_fd_index),
                        SD_JSON_BUILD_PAIR_CONDITION(instance->offset != UINT64_MAX, "offset", SD_JSON_BUILD_UNSIGNED (instance->offset)),
                        SD_JSON_BUILD_PAIR_CONDITION(instance->size != UINT64_MAX, "maxSize", SD_JSON_BUILD_UNSIGNED (instance->size)));
                if (r < 0)
                        return r;
        }

        const char *error_id = NULL;
        sd_json_variant *reply = NULL, *d = NULL;
        r = varlink_callbo_and_log(
                pull_link,
                "io.systemd.PullJob.PullFile",
                &reply,
                &error_id,
                SD_JSON_BUILD_PAIR_CONDITION(iovec_is_set(&j->expected_checksum), "expectedChecksum", SD_JSON_BUILD_STRING (hexmem(j->expected_checksum.iov_base, j->expected_checksum.iov_len))),
                SD_JSON_BUILD_PAIR_STRING("source", j->url),
                SD_JSON_BUILD_PAIR_UNSIGNED("destinationFileDescriptor", destination_fd_index),
                SD_JSON_BUILD_PAIR_CONDITION(j->instances != NULL, "instances", SD_JSON_BUILD_VARIANT(instances_array)),
                SD_JSON_BUILD_PAIR_CONDITION(FILE_SIZE_VALID(j->offset), "offset", SD_JSON_BUILD_UNSIGNED(j->offset)),
                SD_JSON_BUILD_PAIR_CONDITION(FILE_SIZE_VALID(j->uncompressed_max), "maxSize", SD_JSON_BUILD_UNSIGNED(j->uncompressed_max)),
                SD_JSON_BUILD_PAIR_CONDITION(j->old_etags != NULL, "oldEtags", SD_JSON_BUILD_STRV(j->old_etags)));
        if (r < 0)
                return pull_job_finish(j, r);

        d = sd_json_variant_by_key(reply, "etagExists");
        if (!d)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "PullFile() response is missing 'etagExists' key.");

        if (!sd_json_variant_is_boolean(d))
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "PullFile() response 'etagExists' field not a boolean");

        j->etag_exists = sd_json_variant_boolean(d);

        d = sd_json_variant_by_key(reply, "etag");
        if (d && !sd_json_variant_is_null(d)) {
                if (!sd_json_variant_is_string(d))
                        return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                               "PullFile() response 'etag' field not a string");

                j->etag = strdup(sd_json_variant_string(d));
                if (!j->etag)
                        return log_oom();
        }

        d = sd_json_variant_by_key(reply, "checksum");
        if (!d)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "PullFile() response is missing 'checksum' key.");

        if (!sd_json_variant_is_string(d))
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "PullFile() response 'checksum' field not a string");

        r = sd_json_variant_unhex (d, &j->checksum.iov_base, &j->checksum.iov_len);
        if (r < 0)
                return r;

        return pull_job_finish(j, 0);
}

void pull_job_close_disk_fd(PullJob *j) {
        if (!j)
                return;

        safe_close(j->disk_fd);

        j->disk_fd = -EBADF;
}

PullJob* pull_job_unref(PullJob *j) {
        if (!j)
                return NULL;

        pull_job_close_disk_fd(j);

        free(j->url);
        free(j->etag);
        strv_free(j->old_etags);
        iovec_done(&j->checksum);
        iovec_done(&j->expected_checksum);

        return mfree(j);
}

int pull_job_finish(PullJob *j, int ret) {
        assert(j);

        if (IN_SET(j->state, PULL_JOB_DONE, PULL_JOB_FAILED))
                return 0;

        if (ret == 0) {
                j->state = PULL_JOB_DONE;
                j->progress_percent = 100;
                log_info("Download of %s complete.", j->url);
        } else {
                j->state = PULL_JOB_FAILED;
                j->error = ret;
        }

        if (j->on_finished)
                return j->on_finished(j);
        else
                return 0;
}

#include "time-util.h"

int pull_job_new(
                PullJob **ret,
                const char *url,
                void *userdata) {

        _cleanup_(pull_job_unrefp) PullJob *j = NULL;
        _cleanup_free_ char *u = NULL;

        assert(url);
        assert(ret);

        u = strdup(url);
        if (!u)
                return -ENOMEM;

        j = new(PullJob, 1);
        if (!j)
                return -ENOMEM;

        *j = (PullJob) {
                .state = PULL_JOB_INIT,
                .disk_fd = -EBADF,
                .userdata = userdata,
                .uncompressed_max = 64LLU * 1024LLU * 1024LLU * 1024LLU, /* 64GB safety limit */
                .url = TAKE_PTR(u),
                .offset = UINT64_MAX,
                .sync = true,
                .instances = NULL,
                .n_instances = 0
        };

        *ret = TAKE_PTR(j);

        return 0;
}
