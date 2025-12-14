/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <locale.h>

#include "curl-util.h"
#include "fd-util.h"
#include "json-util.h"
#include "io-util.h"
#include "log.h"
#include "main-func.h"
#include "pull-job.h"
#include "sd-event.h"
#include "signal-util.h"
#include "varlink-io.systemd.PullJob.h"
#include "varlink-util.h"

typedef struct MethodPullParameters {
        const char *source;
        unsigned destination_fd_index;
        uint64_t offset;
        uint64_t size_max;
} MethodPullParameters;

static void pull_job_on_finished(PullJob *job) {
        //
}

static int pull_job_on_open_disk(PullJob *job) {
        return 0;
}

static void pull_job_on_progress(PullJob *job) {
        //
}

static int pull_raw(MethodPullParameters params, int destination_fd) {
        _cleanup_(pull_job_unrefp) PullJob *pull_job = NULL;
        _cleanup_(curl_glue_unrefp) CurlGlue *glue = NULL;
        _cleanup_(sd_event_unrefp) sd_event *event = NULL;
        int r;

        r = sd_event_default(&event);
        if (r < 0)
                return r;

        r = curl_glue_new(&glue, event);
        if (r < 0)
                return r;
        glue->on_finished = pull_job_curl_on_finished;

        pull_job_new (&pull_job, params.source, glue, /* userdata= */ NULL);

        pull_job->on_finished = pull_job_on_finished;
        pull_job->on_open_disk = pull_job_on_open_disk;
        pull_job->calc_checksum = true;
        pull_job->force_memory = false;

        if (params.size_max != UINT64_MAX)
                pull_job->uncompressed_max = params.size_max;
        if (params.offset != UINT64_MAX)
                pull_job->offset = params.offset;

        pull_job->on_progress = pull_job_on_progress;
        pull_job->sync = false; //do on the caller side

        r = pull_job_begin(pull_job);
        if (r < 0)
                return r;

        return 0;
}

static int vl_method_pull_raw(sd_varlink *link, sd_json_variant *parameters, sd_varlink_method_flags_t flags, void *userdata) {

        // parse only the parameters used by systemd-pull

        static const sd_json_dispatch_field dispatch_table[] = {
                { "source",         SD_JSON_VARIANT_STRING,  sd_json_dispatch_const_string, offsetof(MethodPullParameters, source),      SD_JSON_MANDATORY },
                { "destinationFileDescriptor", _SD_JSON_VARIANT_TYPE_INVALID, sd_json_dispatch_uint, offsetof(MethodPullParameters, destination_fd_index), SD_JSON_MANDATORY },
                { "instances",      SD_JSON_VARIANT_ARRAY,   NULL,                          0,                                           0 },
                { "offset",         SD_JSON_VARIANT_NUMBER,  sd_json_dispatch_uint64,       offsetof(MethodPullParameters, offset),      0 },
                { "maxSize",        SD_JSON_VARIANT_NUMBER,  sd_json_dispatch_uint64,       offsetof(MethodPullParameters, size_max),    0 },
                {}
        };

        MethodPullParameters p = {
                .destination_fd_index = UINT_MAX,
                .offset = UINT64_MAX,
                .size_max = UINT64_MAX,
        };
        int r;

        assert(link);

        r = sd_varlink_dispatch(link, parameters, dispatch_table, &p);
        if (r != 0)
                return r;

        int destination_fd = sd_varlink_take_fd(link, p.destination_fd_index);
        if (destination_fd < 0)
                return sd_varlink_error(link, "io.systemd.PullJob.InvalidParameters", NULL);

        if (p.offset != UINT64_MAX && !FILE_SIZE_VALID(p.offset))
                return sd_varlink_error(link, "io.systemd.PullJob.InvalidParameters", NULL);

        if (p.size_max != UINT64_MAX && (!FILE_SIZE_VALID(p.size_max) || (p.size_max % 1024) != 0))
                return sd_varlink_error(link, "io.systemd.PullJob.InvalidParameters", NULL);

        /* Make sure offset+size is still in the valid range if both set */
        if (p.offset != UINT64_MAX && p.size_max != UINT64_MAX &&
            ((p.size_max > (UINT64_MAX - p.offset)) ||
             !FILE_SIZE_VALID(p.offset + p.size_max)))
                return sd_varlink_error(link, "io.systemd.PullJob.InvalidParameters", NULL);

        r = pull_raw(p, TAKE_FD(destination_fd));
        if (r < 0)
                return sd_varlink_error(link, "io.systemd.PullJob.PullError", NULL);

        return sd_varlink_reply(link, NULL);
}

static int vl_server(void) {
        _cleanup_(sd_varlink_server_unrefp) sd_varlink_server *varlink_server = NULL;
        int r;

        r = varlink_server_new(&varlink_server, SD_VARLINK_SERVER_ALLOW_FD_PASSING_INPUT, /* userdata= */ NULL);
        if (r < 0)
                return log_error_errno(r, "Failed to allocate Varlink server: %m");

        r = sd_varlink_server_add_interface(varlink_server, &vl_interface_io_systemd_PullJob);
        if (r < 0)
                return log_error_errno(r, "Failed to add Varlink interface: %m");

        r = sd_varlink_server_bind_method(varlink_server, "io.systemd.PullJob.PullRaw", vl_method_pull_raw);
        if (r < 0)
                return log_error_errno(r, "Failed to bind Varlink method: %m");

        r = sd_varlink_server_loop_auto(varlink_server);
        if (r < 0)
                return log_error_errno(r, "Failed to run Varlink event loop: %m");

        return 0;
}

static int run(int argc, char *argv[]) {
        int r;

        setlocale(LC_ALL, "");
        log_setup();

        (void) ignore_signals(SIGPIPE);

        r = sd_varlink_invocation(SD_VARLINK_ALLOW_ACCEPT);
        if (r < 0)
                return log_error_errno(r, "Failed to check if invoked in Varlink mode: %m");
        if (r > 0)
                return vl_server(); /* Invocation as Varlink service */

        return 0;
}

DEFINE_MAIN_FUNCTION(run);
