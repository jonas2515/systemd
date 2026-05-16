/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <fcntl.h>
#include <linux/magic.h>
#include <sys/stat.h>
#include <unistd.h>

#include "alloc-util.h"
#include "blockdev-util.h"
#include "build-path.h"
#include "chase.h"
#include "device-util.h"
#include "devnum-util.h"
#include "dirent-util.h"
#include "env-util.h"
#include "errno-util.h"
#include "fd-util.h"
#include "fdisk-util.h"
#include "fileio.h"
#include "find-esp.h"
#include "fs-util.h"
#include "glyph-util.h"
#include "gpt.h"
#include "hexdecoct.h"
#include "import-util.h"
#include "iovec-util.h"
#include "pidref.h"
#include "io-util.h"
#include "memfd-util.h"
#include "process-util.h"
#include "sd-varlink.h"
#include "sort-util.h"
#include "stat-util.h"
#include "string-table.h"
#include "strv.h"
#include "sysupdate-installer-backend.h"
#include "time-util.h"
#include "tmpfile-util.h"
#include "utf8.h"
#include "varlink-util.h"

#define INSTALLER_BACKENDS_DIRECTORY "/run/systemd/io.systemd.Sysupdate.Installer/"

// copied from import/pull-worker-varlink.c
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

int installer_backend_call_list_available_instances(
                const char *url,
                SysupdateInstallerBackendAvailableInstance **ret_instances,
                size_t *ret_n_instances) {

        int r;

        assert(url);
        assert(ret_instances);
        assert(ret_n_instances);

        _cleanup_free_ const char *protocol;
        r = url_get_protocol(url, &protocol);
        if (r < 0)
                return log_error_errno(r, "Failed to parse protocol from URL %s: %m", url);

        _cleanup_(sd_varlink_flush_close_unrefp) sd_varlink *vl;
        _cleanup_free_ const char *socket_path = path_join(INSTALLER_BACKENDS_DIRECTORY, protocol);
        r = sd_varlink_connect_address(&vl, socket_path);
        if (r < 0)
                return log_error_errno(r, "Failed to connect to installer backend at varlink socket '%s': %m", socket_path);

        r = sd_varlink_set_allow_fd_passing_input(vl, true);
        if (r < 0)
                return log_error_errno(r, "Failed to enable FD passing output on varlink: %m");

        sd_json_variant *reply = NULL, *d = NULL;
        r = varlink_callbo_and_log(
                vl,
                "io.systemd.Sysupdate.Installer.ListAvailableInstances",
                &reply,
                SD_JSON_BUILD_PAIR_STRING("url", url));
        if (r < 0)
                return r;

        d = sd_json_variant_by_key(reply, "availableInstances");
        if (!d)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "ListInstances() response is missing 'availableInstances' key.");

        if (!sd_json_variant_is_array(d))
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "ListInstances() response 'availableInstances' field not an array");

        size_t n_instances = sd_json_variant_elements(d);

        SysupdateInstallerBackendAvailableInstance *items = new(SysupdateInstallerBackendAvailableInstance, n_instances);
        if (!items)
                return log_oom();

        CLEANUP_ARRAY(items, n_instances, sysupdate_installer_backend_available_instances_free);

        for (size_t i = 0; i < n_instances; i++) {
                sd_json_variant *elem = sd_json_variant_by_index(d, i);
                if (!elem || !sd_json_variant_is_object(elem))
                        return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                              "ListInstances() instance at index %zu is not an object", i);

                sd_json_variant *name_var = sd_json_variant_by_key(elem, "name");
                if (!name_var || !sd_json_variant_is_string(name_var))
                        return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                              "ListInstances() instance at index %zu missing or invalid 'name' field", i);

                sd_json_variant *fd_index_var = sd_json_variant_by_key(elem, "descriptorFdIndex");
                if (!fd_index_var || !sd_json_variant_is_integer(fd_index_var))
                        return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                              "ListInstances() instance at index %zu missing or invalid 'descriptorFdIndex' field", i);

                const char *name = sd_json_variant_string(name_var);
                int64_t fd_index = sd_json_variant_integer(fd_index_var);

                if (fd_index < 0)
                        return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                              "ListInstances() instance (name %s) has invalid fd_index value: %ld", name, fd_index);

                _cleanup_close_ int available_instance_fd = sd_varlink_take_fd(vl, fd_index);
                if (available_instance_fd < 0)
                        return log_error_errno(available_instance_fd, "Failed to get FD at index %ld from varlink: %m", fd_index);

                // FIXME: will systemd free the variant itself?
                items[i].name = strdup(name);
                if (!items[i].name)
                        return log_oom();

                items[i].available_instance_fd = TAKE_FD(available_instance_fd);
                items[i].prepared_available_instance_fd = -EBADF;
        }

        *ret_instances = TAKE_PTR(items);
        *ret_n_instances = n_instances;
        return 0;
}

int installer_backend_call_prepare_install_instance(
                const char *url,
                int write_target_fd,
                int write_target_offset,
                int write_target_max_size,
                int existing_instance_fd,
                int existing_instance_offset,
                int existing_instance_max_size,
                SysupdateInstallerBackendAvailableInstance *avail_instance) {

        int r;
        int prepared_available_instance_fd;

        assert(url);
        assert(write_target_fd >= 0);
        assert(avail_instance);
        assert(avail_instance->available_instance_fd >= 0);

        const char *protocol;
        r = url_get_protocol(url, &protocol);
        if (r < 0)
                return log_error_errno(r, "Failed to parse protocol from URL %s: %m", url);

        _cleanup_(sd_varlink_flush_close_unrefp) sd_varlink *vl;
        r = sd_varlink_connect_address(&vl, path_join(INSTALLER_BACKENDS_DIRECTORY, protocol));
        if (r < 0)
                return log_error_errno(r, "Failed to connect to sysupdate installer backend '%s': %m", protocol);

        r = sd_varlink_set_allow_fd_passing_output(vl, 1);
        if (r < 0)
                return log_error_errno(r, "Failed to enable FD passing output on varlink: %m");

        r = sd_varlink_set_allow_fd_passing_input(vl, 1);
        if (r < 0)
                return log_error_errno(r, "Failed to enable FD passing input on varlink: %m");

        r = sd_varlink_push_fd(vl, TAKE_FD(avail_instance->available_instance_fd));
        if (r < 0)
                return log_error_errno(r, "Failed to push available instance FD for varlink: %m");

        r = sd_varlink_push_fd(vl, TAKE_FD(write_target_fd));
        if (r < 0)
                return log_error_errno(r, "Failed to push write target FD for varlink: %m");

        _cleanup_(sd_json_variant_unrefp) sd_json_variant *write_target = NULL;
        r = sd_json_build(&write_target,
                SD_JSON_BUILD_OBJECT(
                        SD_JSON_BUILD_PAIR_INTEGER("offset", write_target_offset),
                        SD_JSON_BUILD_PAIR_INTEGER("maxSize", write_target_max_size),
                        SD_JSON_BUILD_PAIR_INTEGER("fdIndex", 1)
                )
        );
        if (r < 0)
                return log_error_errno(r, "Failed to build write_target: %m");

        _cleanup_(sd_json_variant_unrefp) sd_json_variant *existing_instances = NULL;
        if (existing_instance_fd >= 0) {
                r = sd_varlink_push_fd(vl, TAKE_FD(existing_instance_fd));
                if (r < 0)
                        return log_error_errno(r, "Failed to push existing instance FD for varlink: %m");

                r = sd_json_build(&existing_instances,
                        SD_JSON_BUILD_ARRAY(
                                SD_JSON_BUILD_OBJECT(
                                        SD_JSON_BUILD_PAIR_INTEGER("offset", existing_instance_offset),
                                        SD_JSON_BUILD_PAIR_INTEGER("maxSize", existing_instance_max_size),
                                        SD_JSON_BUILD_PAIR_INTEGER("fdIndex", 2)
                                )
                        )
                );
        } else {
                r = sd_json_build(&existing_instances,
                        SD_JSON_BUILD_EMPTY_ARRAY
                );
        }
        if (r < 0)
                return log_error_errno(r, "Failed to build existing_instances array: %m");

        sd_json_variant *reply = NULL;
        r = varlink_callbo_and_log(
                vl,
                "io.systemd.Sysupdate.Installer.PrepareInstallInstance",
                &reply,
                SD_JSON_BUILD_PAIR_VARIANT("writeTarget", write_target),
                SD_JSON_BUILD_PAIR_VARIANT("existingInstances", existing_instances));
        if (r < 0)
                return r;

        prepared_available_instance_fd = sd_varlink_take_fd(vl, 0);
        if (prepared_available_instance_fd < 0)
                return log_error_errno(prepared_available_instance_fd, "Failed to get FD at index 0 from varlink: %m");

        avail_instance->prepared_available_instance_fd = TAKE_FD(prepared_available_instance_fd);

        return 0;
}

int installer_backend_call_install_instance(
                const char *url,
                int write_target_fd,
                int write_target_offset,
                int write_target_max_size,
                int existing_instance_fd,
                int existing_instance_offset,
                int existing_instance_max_size,
                SysupdateInstallerBackendAvailableInstance *avail_instance) {

        int r;

        assert(url);
        assert(write_target_fd >= 0);
        assert(avail_instance);
        assert(avail_instance->prepared_available_instance_fd >= 0);

        const char *protocol;
        r = url_get_protocol(url, &protocol);
        if (r < 0)
                return log_error_errno(r, "Failed to parse protocol from URL %s: %m", url);

        _cleanup_(sd_varlink_flush_close_unrefp) sd_varlink *vl;
        r = sd_varlink_connect_address(&vl, path_join(INSTALLER_BACKENDS_DIRECTORY, protocol));
        if (r < 0)
                return log_error_errno(r, "Failed to connect to sysupdate installer backend '%s': %m", protocol);

        r = sd_varlink_set_allow_fd_passing_output(vl, 1);
        if (r < 0)
                return log_error_errno(r, "Failed to enable FD passing output on varlink: %m");

        r = sd_varlink_push_fd(vl, TAKE_FD(avail_instance->prepared_available_instance_fd));
        if (r < 0)
                return log_error_errno(r, "Failed to push prepared FD for varlink: %m");

        r = sd_varlink_push_fd(vl, TAKE_FD(write_target_fd));
        if (r < 0)
                return log_error_errno(r, "Failed to push write target FD for varlink: %m");

        _cleanup_(sd_json_variant_unrefp) sd_json_variant *write_target = NULL;
        r = sd_json_build(&write_target,
                SD_JSON_BUILD_OBJECT(
                        SD_JSON_BUILD_PAIR_INTEGER("offset", write_target_offset),
                        SD_JSON_BUILD_PAIR_INTEGER("maxSize", write_target_max_size),
                        SD_JSON_BUILD_PAIR_INTEGER("fdIndex", 1)
                )
        );
        if (r < 0)
                return log_error_errno(r, "Failed to build write_target: %m");

        _cleanup_(sd_json_variant_unrefp) sd_json_variant *existing_instances = NULL;
        if (existing_instance_fd >= 0) {
                r = sd_varlink_push_fd(vl, TAKE_FD(existing_instance_fd));
                if (r < 0)
                        return log_error_errno(r, "Failed to push existing instance FD for varlink: %m");

                r = sd_json_build(&existing_instances,
                        SD_JSON_BUILD_ARRAY(
                                SD_JSON_BUILD_OBJECT(
                                        SD_JSON_BUILD_PAIR_INTEGER("offset", existing_instance_offset),
                                        SD_JSON_BUILD_PAIR_INTEGER("maxSize", existing_instance_max_size),
                                        SD_JSON_BUILD_PAIR_INTEGER("fdIndex", 2)
                                )
                        )
                );
        } else {
                r = sd_json_build(&existing_instances,
                        SD_JSON_BUILD_EMPTY_ARRAY
                );
        }
        if (r < 0)
                return log_error_errno(r, "Failed to build existing_instances array: %m");

        sd_json_variant *reply = NULL;
        r = varlink_callbo_and_log(
                vl,
                "io.systemd.Sysupdate.Installer.InstallInstance",
                &reply,
                SD_JSON_BUILD_PAIR_VARIANT("writeTarget", write_target),
                SD_JSON_BUILD_PAIR_VARIANT("existingInstances", existing_instances));
        if (r < 0)
                return r;

        return 0;
}
