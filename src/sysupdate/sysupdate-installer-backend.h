/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include "fd-util.h"

typedef struct {
        char *name;
        int available_instance_fd;
        int prepared_available_instance_fd;
} SysupdateInstallerBackendAvailableInstance;

static void sysupdate_installer_backend_available_instance_done(SysupdateInstallerBackendAvailableInstance *avail_instance) {
        if (avail_instance->name)
                free(avail_instance->name);
        safe_close(avail_instance->available_instance_fd);
        safe_close(avail_instance->prepared_available_instance_fd);
}

static SysupdateInstallerBackendAvailableInstance * sysupdate_installer_backend_available_instance_unref(SysupdateInstallerBackendAvailableInstance *avail_instance) {
        sysupdate_installer_backend_available_instance_done(avail_instance);

        return mfree(avail_instance);
}

DEFINE_TRIVIAL_CLEANUP_FUNC(SysupdateInstallerBackendAvailableInstance*, sysupdate_installer_backend_available_instance_unref);
static DEFINE_ARRAY_FREE_FUNC(sysupdate_installer_backend_available_instances_free, SysupdateInstallerBackendAvailableInstance, sysupdate_installer_backend_available_instance_done);

int installer_backend_call_list_available_instances(const char *url, SysupdateInstallerBackendAvailableInstance **ret_instances, size_t *ret_n_instances);

int installer_backend_call_prepare_install_instance(const char *url,
                                                    int write_target_fd,
                                                    int write_target_offset,
                                                    int write_target_max_size,
                                                    int existing_instance_fd,
                                                    int existing_instance_offset,
                                                    int existing_instance_max_size,
                                                    SysupdateInstallerBackendAvailableInstance *avail_instance);

int installer_backend_call_install_instance(const char *url,
                                            int write_target_fd,
                                            int write_target_offset,
                                            int write_target_max_size,
                                            int existing_instance_fd,
                                            int existing_instance_offset,
                                            int existing_instance_max_size,
                                        SysupdateInstallerBackendAvailableInstance *avail_instance);