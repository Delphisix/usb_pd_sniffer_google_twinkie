/* Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/* Version number for Chrome EC */

#ifndef __CROS_EC_VERSION_H
#define __CROS_EC_VERSION_H

#include "common.h"

#define CROS_EC_IMAGE_DATA_COOKIE1 0xce778899
#define CROS_EC_IMAGE_DATA_COOKIE2 0xceaabbdd

struct image_data {
	uint32_t cookie1;
	char version[32];
	uint32_t size;
	int32_t rollback_version;
	uint32_t cookie2;
} __packed;

extern const struct image_data current_image_data;
extern const char build_info[];
extern const char __image_data_offset[];
extern const void *__image_size;

/**
 * Get the number of commits field from version string.
 */
uint32_t ver_get_numcommits(void);
#endif  /* __CROS_EC_VERSION_H */
