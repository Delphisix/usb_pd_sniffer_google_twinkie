/* Copyright 2016 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef __EC_INCLUDE_NVMEM_VARS_H
#define __EC_INCLUDE_NVMEM_VARS_H

/*
 * CONFIG_FLASH_NVMEM provides persistent, atomic-update storage in
 * flash. The storage is logically divided into one or more "user regions", as
 * configured in board.h and board.c
 *
 * CONFIG_FLASH_NVMEM_VARS stores a set of <KEY, VALUE> tuples in the nvmem
 * user region designated by CONFIG_FLASH_NVMEM_VARS_USER_NUM (in board.h)
 *
 * Tuples are stored and managed using this struct:
 */

struct tuple {
	uint8_t key_len;			/* 1 - 255 */
	uint8_t val_len;			/* 1 - 255 */
	uint8_t flags;				/* RESERVED, will be zeroed */
	uint8_t data_[0];			/* Opaque. Don't look here. */
};

/*
 * Both KEY and VALUE can be any binary blob between 1 and 255 bytes (flash
 * memory is limited, so if you need longer values just use two keys and
 * concatenate the blobs). Zero-length KEYs or VALUEs are not allowed.
 * Assigning a zero-length VALUE to a KEY just deletes that tuple (if it
 * existed).
 *
 * The expected usage is:
 *
 * 1. At boot, call initvars() to ensure that the variable storage region is
 *    valid. If it isn't, this will initialize it to an empty set.
 *
 * 2. Call getenv() or setenv() as needed. The first call to either will copy
 *    the storage regsion from flash into a RAM buffer. Any changes made with
 *    setenv() will affect only that RAM buffer.
 *
 * 3. Call writevars() to commit the RAM buffer to flash and free it.
 *
 * CAUTION: The underlying CONFIG_FLASH_NVMEM implementation allows access by
 * multiple tasks, provided each task access only one user region. There is no
 * support for simultaneous access to the *same* user region by multiple tasks.
 * CONFIG_FLASH_NVMEM_VARS stores all variables in one user region, so if
 * variable access by multiple tasks is required, the tasks should establish
 * their own locks or mutexes to fit their usage. In general that would mean
 * aquiring a lock before calling getvar() or setvar(), and releasing it after
 * calling writevars().
 */

/*
 * Initialize the persistent storage. This checks the user region to ensure
 * that all tuples are valid and that there is one additional '\0' at the end.
 * If any discrepancies are found, it erases all values. This should return
 * EC_SUCCESS unless there is a problem writing to flash.
 */
int initvars(void);

/*
 * Look up a key, return a pointer to the tuple. If the key is not found,
 * return NULL. WARNING: The returned pointer is only valid until the next call
 * to setvar() or writevars(). Use it or lose it.
 */
const struct tuple *getvar(const uint8_t *key, uint8_t key_len);

/* Use these to access the data components of a valid struct tuple pointer */
const uint8_t *tuple_key(const struct tuple *);
const uint8_t *tuple_val(const struct tuple *);

/*
 * Save the tuple in the RAM buffer. If val is NULL or val_len is 0, the
 * tuple is deleted (if it existed). Returns EC_SUCCESS or error code.
 */
int setvar(const uint8_t *key, uint8_t key_len,
	   const uint8_t *val, uint8_t val_len);

/*
 * Commit any changes made with setvar() to persistent memory, and invalidate
 * the RAM buffer. Return EC_SUCCESS or error code on failure.
 */
int writevars(void);

#endif	/* __EC_INCLUDE_NVMEM_VARS_H */
