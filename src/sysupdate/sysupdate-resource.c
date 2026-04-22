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
#include "glyph-util.h"
#include "gpt.h"
#include "hexdecoct.h"
#include "import-util.h"
#include "iovec-util.h"
#include "pidref.h"
#include "process-util.h"
#include "sort-util.h"
#include "stat-util.h"
#include "string-table.h"
#include "strv.h"
#include "sysupdate-cache.h"
#include "sysupdate-installer-backend.h"
#include "sysupdate-instance.h"
#include "sysupdate-partition.h"
#include "sysupdate-pattern.h"
#include "sysupdate-resource.h"
#include "time-util.h"
#include "utf8.h"

void resource_destroy(Resource *rr) {
        assert(rr);

        free(rr->path);
        strv_free(rr->patterns);

        for (size_t i = 0; i < rr->n_instances; i++)
                instance_free(rr->instances[i]);
        free(rr->instances);
}

static int resource_add_instance(
                Resource *rr,
                const char *path,
                const InstanceMetadata *f,
                Instance **ret) {

        Instance *i;
        int r;

        assert(rr);
        assert(path);
        assert(f);
        assert(f->version);

        if (!GREEDY_REALLOC(rr->instances, rr->n_instances + 1))
                return log_oom();

        r = instance_new(rr, path, f, &i);
        if (r < 0)
                return r;

        rr->instances[rr->n_instances++] = i;

        if (ret)
                *ret = i;

        return 0;
}

static int resource_load_from_directory_recursive(
                Resource *rr,
                DIR* d,
                const char* relpath,
                const char* relpath_for_matching,
                mode_t m,
                bool ancestor_is_partial,
                bool ancestor_is_pending) {
        int r;

        for (;;) {
                _cleanup_(instance_metadata_destroy) InstanceMetadata extracted_fields = INSTANCE_METADATA_NULL;
                _cleanup_free_ char *joined = NULL, *rel_joined = NULL;
                _cleanup_free_ char *rel_joined_for_matching = NULL;
                Instance *instance;
                struct dirent *de;
                const char *de_d_name_stripped;
                struct stat st;
                bool is_partial = ancestor_is_partial, is_pending = ancestor_is_pending;
                const char *stripped;

                errno = 0;
                de = readdir_no_dot(d);
                if (!de) {
                        if (errno != 0)
                                return log_error_errno(errno, "Failed to read directory '%s': %m", rr->path);
                        break;
                }

                switch (de->d_type) {

                case DT_UNKNOWN:
                        break;

                case DT_DIR:
                        if (!IN_SET(m, S_IFDIR, S_IFREG))
                                continue;

                        break;

                case DT_REG:
                        if (m != S_IFREG)
                                continue;
                        break;

                default:
                        continue;
                }

                if (fstatat(dirfd(d), de->d_name, &st, AT_NO_AUTOMOUNT) < 0) {
                        if (errno == ENOENT) /* Gone by now? */
                                continue;

                        return log_error_errno(errno, "Failed to stat %s/%s: %m", rr->path, de->d_name);
                }

                if (!(S_ISDIR(st.st_mode) && S_ISREG(m)) && ((st.st_mode & S_IFMT) != m))
                        continue;

                if ((stripped = startswith(de->d_name, ".sysupdate.partial."))) {
                        de_d_name_stripped = stripped;
                        is_partial = true;
                } else if ((stripped = startswith(de->d_name, ".sysupdate.pending."))) {
                        de_d_name_stripped = stripped;
                        is_pending = true;
                } else
                        de_d_name_stripped = de->d_name;

                rel_joined = path_join(relpath, de->d_name);
                if (!rel_joined)
                        return log_oom();

                /* Match against the filename with any `.sysupdate.partial.` (etc.) prefix stripped, so the
                 * user’s patterns still apply. But don’t use the stripped version in any paths or recursion. */
                rel_joined_for_matching = path_join(relpath_for_matching, de_d_name_stripped);
                if (!rel_joined_for_matching)
                        return log_oom();

                r = pattern_match_many(rr->patterns, rel_joined_for_matching, &extracted_fields);
                if (r == PATTERN_MATCH_RETRY) {
                        _cleanup_closedir_ DIR *subdir = NULL;

                        subdir = xopendirat(dirfd(d), rel_joined, 0);
                        if (!subdir)
                                continue;

                        r = resource_load_from_directory_recursive(rr, subdir, rel_joined, rel_joined_for_matching, m, is_partial, is_pending);
                        if (r < 0)
                                return r;
                        if (r == 0)
                                continue;
                }
                else if (r < 0)
                        return log_error_errno(r, "Failed to match pattern: %m");
                else if (r == PATTERN_MATCH_NO)
                        continue;

                if (de->d_type == DT_DIR && m != S_IFDIR)
                        continue;

                joined = path_join(rr->path, rel_joined);
                if (!joined)
                        return log_oom();

                r = resource_add_instance(rr, joined, &extracted_fields, &instance);
                if (r < 0)
                        return r;

                /* Inherit these from the source, if not explicitly overwritten */
                if (instance->metadata.mtime == USEC_INFINITY)
                        instance->metadata.mtime = timespec_load(&st.st_mtim) ?: USEC_INFINITY;

                if (instance->metadata.mode == MODE_INVALID)
                        instance->metadata.mode = st.st_mode & 0775; /* mask out world-writability and suid and stuff, for safety */

                instance->is_partial = is_partial;
                instance->is_pending = is_pending;
        }

        return 0;
}

static int resource_load_from_directory(
                Resource *rr,
                mode_t m) {
        _cleanup_closedir_ DIR *d = NULL;

        assert(rr);
        assert(IN_SET(rr->type, RESOURCE_TAR, RESOURCE_REGULAR_FILE, RESOURCE_DIRECTORY, RESOURCE_SUBVOLUME));
        assert(IN_SET(m, S_IFREG, S_IFDIR));

        d = opendir(rr->path);
        if (!d) {
                if (errno == ENOENT) {
                        log_debug_errno(errno, "Directory %s does not exist, not loading any resources: %m", rr->path);
                        return 0;
                }

                return log_error_errno(errno, "Failed to open directory '%s': %m", rr->path);
        }

        return resource_load_from_directory_recursive(rr, d, NULL, NULL, m, false, false);
}

static int resource_load_from_blockdev(Resource *rr) {
        _cleanup_(fdisk_unref_contextp) struct fdisk_context *c = NULL;
        _cleanup_(fdisk_unref_tablep) struct fdisk_table *t = NULL;
        size_t n_partitions;
        int r;

        assert(rr);

        r = dlopen_fdisk(LOG_DEBUG);
        if (r < 0)
                return r;

        r = fdisk_new_context_at(AT_FDCWD, rr->path, /* read_only= */ true, /* sector_size= */ UINT32_MAX, &c);
        if (r < 0)
                return log_error_errno(r, "Failed to create fdisk context from '%s': %m", rr->path);

        if (!sym_fdisk_is_labeltype(c, FDISK_DISKLABEL_GPT))
                return log_error_errno(SYNTHETIC_ERRNO(EHWPOISON), "Disk %s has no GPT disk label, not suitable.", rr->path);

        r = sym_fdisk_get_partitions(c, &t);
        if (r < 0)
                return log_error_errno(r, "Failed to acquire partition table: %m");

        n_partitions = sym_fdisk_table_get_nents(t);
        for (size_t i = 0; i < n_partitions; i++)  {
                _cleanup_(instance_metadata_destroy) InstanceMetadata extracted_fields = INSTANCE_METADATA_NULL;
                _cleanup_(partition_info_destroy) PartitionInfo pinfo = PARTITION_INFO_NULL;
                Instance *instance;
                bool is_partial = false, is_pending = false;

                r = read_partition_info(c, t, i, &pinfo);
                if (r < 0)
                        return r;
                if (r == 0) /* not assigned */
                        continue;

                /* Check if partition type matches, either directly or via derived partial/pending type
                 * UUIDs. The derived UUIDs are computed from the configured partition type by hashing it
                 * with a fixed app-specific ID, so we can detect the state without relying on label
                 * prefixes. */
                if (rr->partition_type_set) {
                        sd_id128_t partial_type, pending_type;

                        r = gpt_partition_type_uuid_for_sysupdate_partial(rr->partition_type.uuid, &partial_type);
                        if (r < 0)
                                return log_error_errno(r, "Failed to derive partial partition type UUID: %m");

                        r = gpt_partition_type_uuid_for_sysupdate_pending(rr->partition_type.uuid, &pending_type);
                        if (r < 0)
                                return log_error_errno(r, "Failed to derive pending partition type UUID: %m");

                        if (sd_id128_equal(pinfo.type, partial_type))
                                is_partial = true;
                        else if (sd_id128_equal(pinfo.type, pending_type))
                                is_pending = true;
                        else if (!sd_id128_equal(pinfo.type, rr->partition_type.uuid))
                                continue;
                }

                /* A label of "_empty" means "not used so far" for us */
                if (streq_ptr(pinfo.label, "_empty")) {
                        rr->n_empty++;
                        continue;
                }

                r = pattern_match_many(rr->patterns, pinfo.label, &extracted_fields);
                if (r < 0)
                        return log_error_errno(r, "Failed to match pattern: %m");
                if (IN_SET(r, PATTERN_MATCH_NO, PATTERN_MATCH_RETRY))
                        continue;

                r = resource_add_instance(rr, pinfo.device, &extracted_fields, &instance);
                if (r < 0)
                        return r;

                instance->partition_info = pinfo;
                pinfo = (PartitionInfo) PARTITION_INFO_NULL;

                /* Inherit data from source if not configured explicitly */
                if (!instance->metadata.partition_uuid_set) {
                        instance->metadata.partition_uuid = instance->partition_info.uuid;
                        instance->metadata.partition_uuid_set = true;
                }

                if (!instance->metadata.partition_flags_set) {
                        instance->metadata.partition_flags = instance->partition_info.flags;
                        instance->metadata.partition_flags_set = true;
                }

                if (instance->metadata.read_only < 0)
                        instance->metadata.read_only = instance->partition_info.read_only;

                instance->is_partial = is_partial;
                instance->is_pending = is_pending;
        }

        return 0;
}

static int resource_load_from_web(
                Resource *rr,
                bool verify,
                Hashmap **web_cache) {

        int r;
        SysupdateInstallerBackendAvailableInstance *instances = NULL;
        size_t n_instances = 0;

        assert(rr);

        r = installer_backend_call_list_available_instances(rr->path, &instances, &n_instances);
        if (r < 0)
                return r;

        CLEANUP_ARRAY(instances, n_instances, sysupdate_installer_backend_available_instances_free);

        for (size_t inst_idx = 0; inst_idx < n_instances; inst_idx++) {
                SysupdateInstallerBackendAvailableInstance *avail_inst = &instances[inst_idx];
                _cleanup_(instance_metadata_destroy) InstanceMetadata extracted_fields = INSTANCE_METADATA_NULL;
                Instance *instance;;

                if (!filename_is_valid(avail_inst->name))
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Invalid filename for instance.");
                if (string_has_cc(avail_inst->name, NULL))
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Filename for instance contains control characters, refusing.");

                r = pattern_match_many(rr->patterns, avail_inst->name, &extracted_fields);
                if (r < 0)
                        return log_error_errno(r, "Failed to match pattern: %m");
                if (r == PATTERN_MATCH_YES) {
                        _cleanup_free_ char *path = NULL;

                        r = import_url_append_component(rr->path, avail_inst->name, &path);
                        if (r < 0)
                                return log_error_errno(r, "Failed to build instance URL: %m");

                        r = resource_add_instance(rr, path, &extracted_fields, &instance);
                        if (r < 0)
                                return r;

                        // ugh... no easy way to take ownership of the element so we do it ourselves lol.
                        // if we had gPtrArray we could steal :(
                        instance->avail_instance = new(SysupdateInstallerBackendAvailableInstance, 1);
                        instance->avail_instance->name = TAKE_PTR(avail_inst->name);
                        instance->avail_instance->available_instance_fd = TAKE_FD(avail_inst->available_instance_fd);
                        instance->avail_instance->prepared_available_instance_fd = TAKE_FD(avail_inst->prepared_available_instance_fd);

                        /* Web resources can only be a source, not a target, so
                                * can never be partial or pending. */
                        instance->is_partial = false;
                        instance->is_pending = false;
                }
        }

        return 0;
}

static int instance_cmp(Instance *const*a, Instance *const*b) {
        int r;

        assert(a);
        assert(b);
        assert(*a);
        assert(*b);
        assert((*a)->metadata.version);
        assert((*b)->metadata.version);

        /* Newest version at the beginning */
        r = strverscmp_improved((*a)->metadata.version, (*b)->metadata.version);
        if (r != 0)
                return -r;

        /* Instances don't have to be uniquely named (uniqueness on partition tables is not enforced at all,
         * and since we allow multiple matching patterns not even in directories they are unique). Hence
         * let's order by path as secondary ordering key. */
        return path_compare((*a)->path, (*b)->path);
}

int resource_load_instances(Resource *rr, bool verify, Hashmap **web_cache) {
        int r;

        assert(rr);

        switch (rr->type) {

        case RESOURCE_TAR:
        case RESOURCE_REGULAR_FILE:
                r = resource_load_from_directory(rr, S_IFREG);
                break;

        case RESOURCE_DIRECTORY:
        case RESOURCE_SUBVOLUME:
                r = resource_load_from_directory(rr, S_IFDIR);
                break;

        case RESOURCE_PARTITION:
                r = resource_load_from_blockdev(rr);
                break;

        case RESOURCE_URL_FILE:
        case RESOURCE_URL_TAR:
                r = resource_load_from_web(rr, verify, web_cache);
                break;

        default:
                assert_not_reached();
        }
        if (r < 0)
                return r;

        typesafe_qsort(rr->instances, rr->n_instances, instance_cmp);
        return 0;
}

static int instance_version_match(Instance *const*a, Instance *const*b) {
        assert(a);
        assert(b);
        assert(*a);
        assert(*b);
        assert((*a)->metadata.version);
        assert((*b)->metadata.version);

        /* List is sorted newest-to-oldest */
        return -strverscmp_improved((*a)->metadata.version, (*b)->metadata.version);
}

Instance* resource_find_instance(Resource *rr, const char *version) {
        Instance key = {
                .metadata.version = (char*) version,
        }, *k = &key;

        Instance **found;
        found = typesafe_bsearch(&k, rr->instances, rr->n_instances, instance_version_match);
        if (!found)
                return NULL;

        return *found;
}

static int get_sysext_overlay_block(const char *p, dev_t *ret) {
        int r;

        assert(p);
        assert(ret);

        /* Tries to read the backing device information systemd-sysext puts in the virtual file
         * /usr/.systemd-sysext/backing */

        _cleanup_free_ char *j = path_join(p, ".systemd-sysext");
        if (!j)
                return log_oom_debug();

        _cleanup_close_ int fd = open(j, O_RDONLY|O_DIRECTORY);
        if (fd < 0)
                return log_debug_errno(errno, "Failed to open '%s': %m", j);

        r = fd_is_fs_type(fd, OVERLAYFS_SUPER_MAGIC);
        if (r < 0)
                return log_debug_errno(r, "Failed to determine backing file system of '%s': %m", j);
        if (r == 0)
                return log_debug_errno(SYNTHETIC_ERRNO(ENOTTY), "Backing file system of '%s' is not an overlayfs.", j);

        _cleanup_free_ char *buf = NULL;
        r = read_one_line_file_at(fd, "backing", &buf);
        if (r < 0)
                return log_debug_errno(r, "Failed to read contents of '%s/backing': %m", j);

        r = parse_devnum(buf, ret);
        if (r < 0)
                return log_debug_errno(r, "Failed to parse contents of '%s/backing': %m", j);

        if (major(*ret) == 0) { /* not a block device? */
                *ret = 0;
                return 0;
        }

        (void) block_get_originating(*ret, ret, /* recursive= */ false);
        return 1;
}

int resource_resolve_path(
                Resource *rr,
                const char *root,
                const char *relative_to_directory,
                const char *node) {

        _cleanup_free_ char *p = NULL;
        dev_t d;
        int r;

        assert(rr);

        if (IN_SET(rr->path_relative_to, PATH_RELATIVE_TO_ESP, PATH_RELATIVE_TO_XBOOTLDR, PATH_RELATIVE_TO_BOOT) &&
            !IN_SET(rr->type, RESOURCE_REGULAR_FILE, RESOURCE_DIRECTORY))
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "Paths relative to %s are only allowed for regular-file or directory resources.",
                                       path_relative_to_to_string(rr->path_relative_to));

        if (rr->path_auto) {
                /* NB: If the root mount has been replaced by some form of volatile file system (overlayfs),
                 * the original root block device node is symlinked in /run/systemd/volatile-root. Let's
                 * follow that link here. If that doesn't exist, we check the backing device of "/usr". We
                 * don't actually check the backing device of the root fs "/", in order to support
                 * environments where the root fs is a tmpfs, and the OS itself placed exclusively in
                 * /usr/. */

                if (rr->type != RESOURCE_PARTITION)
                        return log_error_errno(SYNTHETIC_ERRNO(EOPNOTSUPP),
                                               "Automatic root path discovery only supported for partition resources.");

                if (node) { /* If --image= is specified, directly use the loopback device */
                        r = free_and_strdup_warn(&rr->path, node);
                        if (r < 0)
                                return r;

                        return 0;
                }

                if (root)
                        return log_error_errno(SYNTHETIC_ERRNO(EPERM),
                                               "Block device is not allowed when using --root= mode.");

                struct stat orig_root_stats;
                r = RET_NERRNO(stat("/run/systemd/volatile-root", &orig_root_stats));
                if (r < 0) {
                        if (r != -ENOENT)
                                return log_error_errno(r, "Failed to stat /run/systemd/volatile-root: %m");

                        /* volatile-root not found */
                        r = get_block_device_harder("/usr/", &d);
                        if (r == 0) /* Not backed by a block device? Let's see if this is a sysext overlayfs instance */
                                r = get_sysext_overlay_block("/usr/", &d);
                        if (r < 0)
                                return log_error_errno(r, "Failed to determine block device of file system: %m");
                } else if (!S_ISBLK(orig_root_stats.st_mode)) /* symlink was present but not block device */
                        return log_error_errno(SYNTHETIC_ERRNO(ENOTBLK), "/run/systemd/volatile-root is not linked to a block device.");
                else /* symlink was present and a block device */
                        d = orig_root_stats.st_rdev;

        } else if (rr->type == RESOURCE_PARTITION) {
                _cleanup_close_ int fd = -EBADF, real_fd = -EBADF;
                _cleanup_free_ char *resolved = NULL;
                struct stat st;

                r = chase(rr->path, root, CHASE_PREFIX_ROOT|CHASE_TRIGGER_AUTOFS, &resolved, &fd);
                if (r < 0)
                        return log_error_errno(r, "Failed to resolve '%s': %m", rr->path);

                if (fstat(fd, &st) < 0)
                        return log_error_errno(errno, "Failed to stat '%s': %m", resolved);

                if (S_ISBLK(st.st_mode) && root)
                        return log_error_errno(SYNTHETIC_ERRNO(EPERM), "When using --root= or --image= access to device nodes is prohibited.");

                if (S_ISREG(st.st_mode) || S_ISBLK(st.st_mode)) {
                        /* Not a directory, hence no need to find backing block device for the path */
                        free_and_replace(rr->path, resolved);
                        return 0;
                }

                if (!S_ISDIR(st.st_mode))
                        return log_error_errno(SYNTHETIC_ERRNO(ENOTDIR), "Target path '%s' does not refer to regular file, directory or block device, refusing.",  rr->path);

                if (node) { /* If --image= is specified all file systems are backed by the same loopback device, hence shortcut things. */
                        r = free_and_strdup_warn(&rr->path, node);
                        if (r < 0)
                                return r;

                        return 0;
                }

                real_fd = fd_reopen(fd, O_RDONLY|O_CLOEXEC|O_DIRECTORY);
                if (real_fd < 0)
                        return log_error_errno(real_fd, "Failed to convert O_PATH file descriptor for %s to regular file descriptor: %m", rr->path);

                r = get_block_device_harder_fd(real_fd, &d);
                if (r < 0)
                        return log_error_errno(r, "Failed to determine block device of file system: %m");

        } else if (RESOURCE_IS_FILESYSTEM(rr->type)) {
                _cleanup_free_ char *resolved = NULL, *relative_to = NULL;
                ChaseFlags chase_flags = CHASE_NONEXISTENT | CHASE_PREFIX_ROOT | CHASE_TRIGGER_AUTOFS;

                if (rr->path_relative_to == PATH_RELATIVE_TO_EXPLICIT) {
                        assert(relative_to_directory);

                        relative_to = strdup(relative_to_directory);
                        if (!relative_to)
                                return log_oom();
                } else if (rr->path_relative_to == PATH_RELATIVE_TO_ROOT) {
                        relative_to = strdup(empty_to_root(root));
                        if (!relative_to)
                                return log_oom();
                } else { /* boot, esp, or xbootldr */
                        r = 0;
                        if (IN_SET(rr->path_relative_to, PATH_RELATIVE_TO_BOOT, PATH_RELATIVE_TO_XBOOTLDR))
                                r = find_xbootldr_and_warn(root, /* path= */ NULL, /* unprivileged_mode= */ -1, &relative_to, /* ret_fd= */ NULL);
                        if (r == -ENOKEY || rr->path_relative_to == PATH_RELATIVE_TO_ESP)
                                r = find_esp_and_warn(root, /* path= */ NULL, /* unprivileged_mode= */ -1, &relative_to, /* ret_fd= */ NULL);
                        if (r < 0)
                                return log_error_errno(r, "Failed to resolve $BOOT: %m");
                        log_debug("Resolved $BOOT to '%s'", relative_to);

                        /* Since this partition is read from EFI, there should be no symlinks */
                        chase_flags |= CHASE_PROHIBIT_SYMLINKS;
                }

                r = chase(rr->path, relative_to, chase_flags, &resolved, NULL);
                if (r < 0)
                        return log_error_errno(r, "Failed to resolve '%s' (relative to '%s'): %m", rr->path, relative_to);

                free_and_replace(rr->path, resolved);
                return 0;
        } else
                return 0; /* Otherwise assume there's nothing to resolve */

        r = block_get_whole_disk(d, &d);
        if (r < 0)
                return log_error_errno(r, "Failed to find whole disk device for partition backing file system: %m");
        if (r == 0)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "File system is not placed on a partition block device, cannot determine whole block device backing root file system.");

        r = devname_from_devnum(S_IFBLK, d, &p);
        if (r < 0)
                return r;

        if (rr->path)
                log_info("Automatically discovered block device '%s' from '%s'.", p, rr->path);
        else
                log_info("Automatically discovered root block device '%s'.", p);

        free_and_replace(rr->path, p);
        return 1;
}

static const char *resource_type_table[_RESOURCE_TYPE_MAX] = {
        [RESOURCE_URL_FILE]     = "url-file",
        [RESOURCE_URL_TAR]      = "url-tar",
        [RESOURCE_TAR]          = "tar",
        [RESOURCE_PARTITION]    = "partition",
        [RESOURCE_REGULAR_FILE] = "regular-file",
        [RESOURCE_DIRECTORY]    = "directory",
        [RESOURCE_SUBVOLUME]    = "subvolume",
};

DEFINE_STRING_TABLE_LOOKUP(resource_type, ResourceType);

static const char *path_relative_to_table[_PATH_RELATIVE_TO_MAX] = {
        [PATH_RELATIVE_TO_ROOT]     = "root",
        [PATH_RELATIVE_TO_ESP]      = "esp",
        [PATH_RELATIVE_TO_XBOOTLDR] = "xbootldr",
        [PATH_RELATIVE_TO_BOOT]     = "boot",
        [PATH_RELATIVE_TO_EXPLICIT] = "explicit",
};

DEFINE_STRING_TABLE_LOOKUP(path_relative_to, PathRelativeTo);
