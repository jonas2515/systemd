/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "bus-polkit.h"

#include "varlink-io.systemd.Updater.h"

static SD_VARLINK_DEFINE_STRUCT_TYPE(
                AvailableUpdate,
                SD_VARLINK_FIELD_COMMENT("Name describing the available update"),
                SD_VARLINK_DEFINE_FIELD(name, SD_VARLINK_STRING, 0),
                SD_VARLINK_FIELD_COMMENT("Index of FD identifying the update (to pass to PrepareUpdate())"),
                SD_VARLINK_DEFINE_FIELD(fdIndex, SD_VARLINK_INT, 0));

static SD_VARLINK_DEFINE_STRUCT_TYPE(
                ImageTarget,
                SD_VARLINK_FIELD_COMMENT("Offset where the target begins, image must begin at this offset"),
                SD_VARLINK_DEFINE_FIELD(offset, SD_VARLINK_INT, SD_VARLINK_NULLABLE),
                SD_VARLINK_FIELD_COMMENT("Max size of the target, image might end earlier within the target"),
                SD_VARLINK_DEFINE_FIELD(maxSize, SD_VARLINK_INT, SD_VARLINK_NULLABLE),
                SD_VARLINK_FIELD_COMMENT("Index of the FD of the target in passed FDs"),
                SD_VARLINK_DEFINE_FIELD(fdIndex, SD_VARLINK_INT, 0));

static SD_VARLINK_DEFINE_METHOD(
                ListInstances,
                SD_VARLINK_FIELD_COMMENT("URL to look for available updates"),
                SD_VARLINK_DEFINE_INPUT(source, SD_VARLINK_STRING, 0),
                SD_VARLINK_FIELD_COMMENT("Available updates"),
                SD_VARLINK_DEFINE_OUTPUT_BY_TYPE(availableUpdates, AvailableUpdate, SD_VARLINK_ARRAY|SD_VARLINK_NULLABLE));

static SD_VARLINK_DEFINE_METHOD(
                PrepareUpdate,
                SD_VARLINK_FIELD_COMMENT("Image target to install the update into (read-only, to calculate size of download)"),
                SD_VARLINK_DEFINE_INPUT_BY_TYPE(writeTarget, ImageTarget, 0),
                SD_VARLINK_FIELD_COMMENT("Image targets to reuse data from for delta-updating (read-only, to calculate size of download)"),
                SD_VARLINK_DEFINE_INPUT_BY_TYPE(existingImages, ImageTarget, SD_VARLINK_ARRAY|SD_VARLINK_NULLABLE),
                SD_VARLINK_FIELD_COMMENT("Size of download to apply the update"),
                SD_VARLINK_DEFINE_OUTPUT(downloadSizeBytes, SD_VARLINK_INT, 0),
                SD_VARLINK_FIELD_COMMENT("Index of the FD to pass to Update()"),
                SD_VARLINK_DEFINE_OUTPUT(descriptorFdIndex, SD_VARLINK_INT, 0));

static SD_VARLINK_DEFINE_METHOD(
                Update,
                SD_VARLINK_FIELD_COMMENT("Image target to install the update into"),
                SD_VARLINK_DEFINE_INPUT_BY_TYPE(writeTarget, ImageTarget, 0),
                SD_VARLINK_FIELD_COMMENT("Image targets to reuse data from for delta-updating"),
                SD_VARLINK_DEFINE_INPUT_BY_TYPE(existingImages, ImageTarget, SD_VARLINK_ARRAY|SD_VARLINK_NULLABLE));

SD_VARLINK_DEFINE_INTERFACE(
                io_systemd_Updater,
                "io.systemd.Updater",
                SD_VARLINK_INTERFACE_COMMENT("An interface for updating"),
                SD_VARLINK_SYMBOL_COMMENT("A target for an image, either existing image to read from, or to write into for a new image"),
                &vl_type_ImageTarget,
                SD_VARLINK_SYMBOL_COMMENT("List available updates"),
                &vl_method_ListInstances,
                SD_VARLINK_SYMBOL_COMMENT("Prepare an update to get size of download"),
                &vl_method_PrepareUpdate,
                SD_VARLINK_SYMBOL_COMMENT("Apply an update"),
                &vl_method_Update);
