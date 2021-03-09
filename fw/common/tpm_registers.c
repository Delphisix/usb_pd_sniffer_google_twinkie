/* Copyright 2015 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/*
 * This implements the register interface for the TPM SPI Hardware Protocol.
 * The master puts or gets between 1 and 64 bytes to a register designated by a
 * 24-bit address. There is no provision for error reporting at this level.
 */

#include "byteorder.h"
#include "console.h"
#include "device_state.h"
#include "extension.h"
#include "link_defs.h"
#include "nvmem.h"
#include "printf.h"
#include "signed_header.h"
#include "system.h"
#include "system_chip.h"
#include "task.h"
#include "tpm_manufacture.h"
#include "tpm_registers.h"
#include "util.h"
#include "watchdog.h"
#include "wp.h"

/* TPM2 library includes. */
#include "ExecCommand_fp.h"
#include "Platform.h"
#include "_TPM_Init_fp.h"
#include "Manufacture_fp.h"

/****************************************************************************/
/*
 * CAUTION: Variables defined in this in this file are treated specially.
 *
 * As always, initialized variables are placed in the .data section, and
 * uninitialized variables in the .bss section. This saves space in the
 * executable, because the loader can just zero .bss prior to running the
 * program.
 *
 * However, the tpm_reset_request() function will zero the .bss section for THIS
 * FILE and all files in the TPM library. Any uninitialized variables defined in
 * this file that must be preserved across tpm_reset_request() must be placed in
 * a separate section.
 *
 * On the other hand, initialized variables (in the .data section) are NOT
 * affected by tpm_reset_request(), so any variables that should be
 * reinitialized must be dealt with manually in the tpm_reset_request()
 * function. To prevent initialized variables from being added to the TPM
 * library without notice, the compiler will reject any that aren't explicitly
 * flagged.
 */

/* This marks uninitialized variables that tpm_reset_request() should ignore */
#define __preserved __attribute__((section(".bss.noreinit")))

/*
 * This marks initialized variables that tpm_reset_request() may need to reset
 */
#define __initialized __attribute__((section(".data.noreinit")))

/****************************************************************************/

#define CPRINTS(format, args...) cprints(CC_TPM, format, ## args)
#define CPRINTF(format, args...) cprintf(CC_TPM, format, ## args)

/* Register addresses for FIFO mode. */
#define TPM_ACCESS	    (0)
#define TPM_INTF_CAPABILITY (0x14)
#define TPM_STS		    (0x18)
#define TPM_DATA_FIFO	    (0x24)
#define TPM_INTERFACE_ID    (0x30)
#define TPM_DID_VID	    (0xf00)
#define TPM_RID		    (0xf04)
#define TPM_FW_VER	    (0xf90)

#define GOOGLE_VID 0x1ae0
#define GOOGLE_DID 0x0028
#define CR50_RID	0  /* No revision ID yet */

static __preserved uint8_t reset_in_progress;

/* Tpm state machine states. */
enum tpm_states {
	tpm_state_idle,
	tpm_state_ready,
	tpm_state_receiving_cmd,
	tpm_state_executing_cmd,
	tpm_state_completing_cmd,
};

/* A preliminary interface capability register value, will be fine tuned. */
#define IF_CAPABILITY_REG ((3 << 28) | /* TPM2.0 (interface 1.3) */   \
			   (3 << 9) | /* up to 64 bytes transfers. */ \
			   0x15) /* Mandatory set to one. */

/* Volatile registers for FIFO mode */
struct tpm_register_file {
	uint8_t access;
	uint32_t sts;
	uint8_t data_fifo[2048]; /* this might have to be even deeper. */
};

/*
 * Tpm representation. This is a file scope variable, only one locality is
 * supported.
 */
static struct {
	enum tpm_states		  state;
	uint32_t fifo_read_index;   /* for read commands */
	uint32_t fifo_write_index;  /* for write commands */
	struct tpm_register_file  regs;
} tpm_;

/* Bit definitions for some TPM registers. */
enum tpm_access_bits {
	tpm_reg_valid_sts = (1 << 7),
	active_locality = (1 << 5),
	request_use = (1 << 1),
	tpm_establishment = (1 << 0),
};

enum tpm_sts_bits {
	tpm_family_shift = 26,
	tpm_family_mask = ((1 << 2) - 1),  /* 2 bits wide */
	tpm_family_tpm2 = 1,
	reset_establishment_bit = (1 << 25),
	command_cancel = (1 << 24),
	burst_count_shift = 8,
	burst_count_mask = ((1 << 16) - 1),  /* 16 bits wide */
	sts_valid = (1 << 7),
	command_ready = (1 << 6),
	tpm_go = (1 << 5),
	data_avail = (1 << 4),
	expect = (1 << 3),
	self_test_done = (1 << 2),
	response_retry = (1 << 1),
};

/* Used to count bytes read in version string */
static int tpm_fw_ver_index;
/*
 * Used to store the full version string, which includes version of the two RO
 * and two RW regions in the flash as well as the version string of the four
 * cr50 image components. The number is somewhat arbitrary, calculated for the
 * worst case scenario when all compontent trees are 'dirty'.
 */
static uint8_t tpm_fw_ver[80];

/*
 * We need to be able to report firmware version to the host, both RO and RW
 * sections. This copies the information into a static string so that it can be
 * passed to the host a little bit at a time.
 */
static void set_version_string(void)
{
	enum system_image_copy_t active_ro, active_rw;
	size_t offset;

	active_ro = system_get_ro_image_copy();
	active_rw = system_get_image_copy();

	snprintf(tpm_fw_ver, sizeof(tpm_fw_ver), "%s:%d RO_%c:%s",
		 system_get_chip_revision(),
		 system_get_board_version(),
		 (active_ro == SYSTEM_IMAGE_RO ? 'A' : 'B'),
		 system_get_version(active_ro));
	offset = strlen(tpm_fw_ver);
	if (offset == sizeof(tpm_fw_ver) - 1)
		return;

	snprintf(tpm_fw_ver + offset,
		 sizeof(tpm_fw_ver) - offset, " RW_%c:%s",
		 (active_rw == SYSTEM_IMAGE_RW ? 'A' : 'B'),
		 system_get_version(active_rw));
}

static void set_tpm_state(enum tpm_states state)
{
	CPRINTF("state transition from %d to %d\n", tpm_.state, state);
	tpm_.state = state;

	if (state == tpm_state_idle) {
		/* Make sure FIFO is empty. */
		tpm_.fifo_read_index = 0;
		tpm_.fifo_write_index = 0;
	}
}

/*
 * Some TPM registers allow writing of only exactly one bit. This helper
 * function allows to verify that a value is compliant with this
 * requirement
 */
static int single_bit_set(uint32_t value)
{
	return value && !(value & (value - 1));
}

/*
 * NOTE: The put/get functions are called in interrupt context! Don't waste a
 * lot of time here - just copy the data and wake up a task to deal with it
 * later. Although if the implementation mandates a "busy" bit somewhere, you
 * might want to set it now to avoid race conditions with back-to-back
 * interrupts.
 */

static void copy_bytes(uint8_t *dest, uint32_t data_size, uint32_t value)
{
	unsigned i;

	data_size = MIN(data_size, 4);

	for (i = 0; i < data_size; i++)
		dest[i] = (value >> (i * 8)) & 0xff;
}

static void access_reg_write(uint8_t data)
{
	if (!single_bit_set(data)) {
		CPRINTF("%s: attempt to set acces reg to %02x\n",
			__func__, data);
		return;
	}

	switch (data) {
	case request_use:
		/*
		 * No multiple localities supported, let's just always honor
		 * this request.
		 */
		tpm_.regs.access |= active_locality;
		break;

	case active_locality:
		switch (tpm_.state) {
		case tpm_state_ready:
		case tpm_state_idle:
			break;
		default:
			/*
			 * TODO: need to decide what to do if there is a
			 * command in progress.
			 */
			CPRINTF("%s: locality release request in state %d\n",
			__func__, tpm_.state);
			break;
		}
		tpm_.regs.access &= ~active_locality;
		/* No matter what we do, fall into idle state. */
		set_tpm_state(tpm_state_idle);
		break;

	default:
		CPRINTF("%s: attempt to set access reg to an unsupported value"
			" of 0x%02x\n", __func__, data);
		break;
	}
}

/*
 * Process writes into the 'important' sts register bits. Actions on all
 * depends on the current state of the device.
 */
static void sts_reg_write_cr(void)
{
	switch (tpm_.state) {
	case tpm_state_idle:
		set_tpm_state(tpm_state_ready);
		tpm_.regs.sts |= command_ready;
		break;
	case tpm_state_ready:
		tpm_.regs.sts |= command_ready;
		break;
	case tpm_state_completing_cmd:
	case tpm_state_executing_cmd:
	case tpm_state_receiving_cmd:
		set_tpm_state(tpm_state_idle);
		tpm_.regs.sts &= ~command_ready;
		break;
	}
}

static void sts_reg_write_tg(void)
{
	switch (tpm_.state) {
	case tpm_state_completing_cmd:
	case tpm_state_executing_cmd:
	case tpm_state_idle:
	case tpm_state_ready:
		break; /* Ignore setting this bit in these states. */
	case tpm_state_receiving_cmd:
		if (!(tpm_.state & expect)) {
			/* This should trigger actual command execution. */
			set_tpm_state(tpm_state_executing_cmd);
			task_set_event(TASK_ID_TPM, TASK_EVENT_WAKE, 0);
		}
		break;
	}
}

static void sts_reg_write_rr(void)
{
	switch (tpm_.state) {
	case tpm_state_idle:
	case tpm_state_ready:
	case tpm_state_receiving_cmd:
	case tpm_state_executing_cmd:
		break;
	case tpm_state_completing_cmd:
		tpm_.fifo_read_index = 0;
		break;
	}
}

/*
 * TPM_STS register both reports current state machine state and controls some
 * of state machine transitions.
 */
static void sts_reg_write(const uint8_t *data, uint32_t data_size)
{
	uint32_t value = 0;

	data_size = MIN(data_size, 4);
	memcpy(&value, data, data_size);

	/* By definition only one bit can be set at a time. */
	if (!single_bit_set(value)) {
		CPRINTF("%s: attempt to set status reg to %02x\n",
			__func__, value);
		return;
	}

	switch (value) {
	case command_ready:
		sts_reg_write_cr();
		break;
	case tpm_go:
		sts_reg_write_tg();
		break;
	case response_retry:
		sts_reg_write_rr();
		break;
	case command_cancel:
		/* TODO: this also needs to be handled, fall through for now. */
	default:
		CPRINTF("requested to write %08x to sts\n", value);
		break;
	}
}

/* Collect received data in the local buffer and change state accordingly. */
static void fifo_reg_write(const uint8_t *data, uint32_t data_size)
{
	uint32_t packet_size;
	struct tpm_cmd_header *tpmh;

	/*
	 * Make sure we are in the appropriate state, otherwise ignore this
	 * access.
	 */
	if ((tpm_.state == tpm_state_ready) && (tpm_.fifo_write_index == 0))
		set_tpm_state(tpm_state_receiving_cmd);

	if (tpm_.state != tpm_state_receiving_cmd) {
		CPRINTF("%s: ignoring data in state %d\n",
			__func__, tpm_.state);
		return;
	}

	if ((tpm_.fifo_write_index + data_size) > sizeof(tpm_.regs.data_fifo)) {
		CPRINTF("%s: receive buffer overflow: %d in addition to %d\n",
			__func__, data_size, tpm_.fifo_write_index);
		tpm_.fifo_write_index = 0;
		set_tpm_state(tpm_state_ready);
		return;
	}

	/* Copy data into the local buffer. */
	memcpy(tpm_.regs.data_fifo + tpm_.fifo_write_index,
	       data, data_size);

	tpm_.fifo_write_index += data_size;

	/* Verify that size in the header matches the block size */
	if (tpm_.fifo_write_index < 6) {
		tpm_.regs.sts |= expect; /* More data is needed. */
		return;
	}

	tpmh = (struct tpm_cmd_header *)tpm_.regs.data_fifo;
	packet_size = be32toh(tpmh->size);
	if (tpm_.fifo_write_index < packet_size) {
		tpm_.regs.sts |= expect; /* More data is needed. */
		return;
	}

	/* All data has been received, Ready for the 'go' command. */
	tpm_.regs.sts &= ~expect;
}

/* TODO: data_size is between 1 and 64, but is not trustworthy! Don't write
 * past the end of any actual registers if data_size is larger than the spec
 * allows. */
void tpm_register_put(uint32_t regaddr, const uint8_t *data, uint32_t data_size)
{
	uint32_t i;

	if (reset_in_progress)
		return;

	CPRINTF("%s(0x%03x, %d,", __func__, regaddr, data_size);
	for (i = 0; i < data_size && i < 4; i++)
		CPRINTF(" %02x", data[i]);
	if (data_size > 4)
		CPRINTF(" ...");
	CPRINTF(")\n");

	switch (regaddr) {
	case TPM_ACCESS:
		/* This is a one byte register, ignore extra data, if any */
		access_reg_write(data[0]);
		break;
	case TPM_STS:
		sts_reg_write(data, data_size);
		break;
	case TPM_DATA_FIFO:
		fifo_reg_write(data, data_size);
		break;
	case TPM_FW_VER:
		/* Reset read byte count */
		tpm_fw_ver_index = 0;
		break;
	default:
		CPRINTF("%s(0x%06x, %d bytes:", __func__, regaddr, data_size);
		for (i = 0; i < data_size; i++)
			CPRINTF(", %02x", data[i]);
		CPRINTF("\n");
		return;
	}

}

void fifo_reg_read(uint8_t *dest, uint32_t data_size)
{
	uint32_t still_in_fifo = tpm_.fifo_write_index -
		tpm_.fifo_read_index;
	uint32_t tpm_sts;

	data_size = MIN(data_size, still_in_fifo);
	memcpy(dest,
	       tpm_.regs.data_fifo + tpm_.fifo_read_index,
	       data_size);

	tpm_.fifo_read_index += data_size;

	tpm_sts = tpm_.regs.sts;
	tpm_sts &= ~(burst_count_mask << burst_count_shift);
	if (tpm_.fifo_write_index == tpm_.fifo_read_index) {
		tpm_sts &= ~(data_avail | command_ready);
		/* Burst size for the following write requests. */
		tpm_sts |= 63 << burst_count_shift;
	} else {
		/*
		 * Tell the master how much there is to read in the next
		 * burst.
		 */
		tpm_sts |= MIN(tpm_.fifo_write_index -
			       tpm_.fifo_read_index, 63) << burst_count_shift;
	}

	tpm_.regs.sts = tpm_sts;
}


/* TODO: data_size is between 1 and 64, but is not trustworthy! We must return
 * that many bytes, but not leak any secrets if data_size is larger than
 * it should be. Return 0x00 or 0xff or whatever the spec says instead. */
void tpm_register_get(uint32_t regaddr, uint8_t *dest, uint32_t data_size)
{
	int i;

	if (reset_in_progress)
		return;

	CPRINTF("%s(0x%06x, %d)", __func__, regaddr, data_size);
	switch (regaddr) {
	case TPM_DID_VID:
		copy_bytes(dest, data_size, (GOOGLE_DID << 16) | GOOGLE_VID);
		break;
	case TPM_RID:
		copy_bytes(dest, data_size, CR50_RID);
		break;
	case TPM_INTF_CAPABILITY:
		copy_bytes(dest, data_size, IF_CAPABILITY_REG);
		break;
	case TPM_ACCESS:
		copy_bytes(dest, data_size, tpm_.regs.access);
		break;
	case TPM_STS:
		CPRINTF(" %x", tpm_.regs.sts);
		copy_bytes(dest, data_size, tpm_.regs.sts);
		break;
	case TPM_DATA_FIFO:
		fifo_reg_read(dest, data_size);
		break;
	case TPM_FW_VER:
		for (i = 0; i < data_size; i++) {
			/*
			 * Only read while the index remains less than the
			 * maximum allowed version string size.
			 */
			if (tpm_fw_ver_index < sizeof(tpm_fw_ver)) {
				*dest++ = tpm_fw_ver[tpm_fw_ver_index];
				/*
				 * If reached end of string, then don't update
				 * the index so that it will keep pointing at
				 * the end of string character and continue to
				 * fill *dest with 0s.
				 */
				if (tpm_fw_ver[tpm_fw_ver_index] != '\0')
					tpm_fw_ver_index++;
			} else
				/* Not in a valid state, just stuff 0s */
				*dest++ = 0;
		}
		break;
	default:
		CPRINTS("%s(0x%06x, %d) => ??", __func__, regaddr, data_size);
		return;
	}
	CPRINTF("\n");
}

static __preserved interface_restart_func if_restart;
void tpm_register_interface(interface_restart_func interface_restart)
{
	if_restart = interface_restart;
}

static void tpm_init(void)
{
	/* This is more related to TPM task activity than TPM transactions */
	cprints(CC_TASK, "%s", __func__);

	if (system_rolling_reboot_suspected()) {
		cprints(CC_TASK, "%s interrupted", __func__);
		return;
	}

	set_tpm_state(tpm_state_idle);
	tpm_.regs.access = tpm_reg_valid_sts;
	/*
	 * I2CS writes must limit the burstsize to 63 for fifo writes to work
	 * properly. For I2CS fifo writes the first byte is the I2C TPM address
	 * and the next up to 62 bytes are the data to write to that register.
	 */
	tpm_.regs.sts = (tpm_family_tpm2 << tpm_family_shift) |
		(63 << burst_count_shift) | sts_valid;

	/* TPM2 library functions. */
	_plat__Signal_PowerOn();

	/* Create version string to be read by host */
	set_version_string();

	watchdog_reload();

	/*
	 * Make sure NV RAM metadata is initialized, needed to check
	 * manufactured status. This is a speculative call which will have to
	 * be repeated in case the TPM has not been through the manufacturing
	 * sequence yet.
	 *
	 * No harm in calling it twice in that case.
	 */
	_TPM_Init();

	if (!tpm_manufactured()) {
		/*
		 * If tpm has not been manufactured yet - this needs to run on
		 * every startup. It will wipe out NV RAM, among other things.
		 */
		TPM_Manufacture(1);
		_TPM_Init();
		_plat__SetNvAvail();
		tpm_endorse();
	} else {
		_plat__SetNvAvail();
	}

	/* Reinitialize TPM interface. */
	if_restart();
}

size_t tpm_get_burst_size(void)
{
	return (tpm_.regs.sts >> burst_count_shift) & burst_count_mask;
}

#ifdef CONFIG_EXTENSION_COMMAND

/* Recognize both original extension and new vendor-specific command codes */
#define IS_CUSTOM_CODE(code)					\
	((code == CONFIG_EXTENSION_COMMAND) ||			\
	 (code & TPM_CC_VENDOR_BIT_MASK))

static void call_extension_command(struct tpm_cmd_header *tpmh,
				   size_t *total_size)
{
	size_t command_size = be32toh(tpmh->size);
	uint32_t rc;

	/*
	 * Note that we don't look for TPM_CC_VENDOR_CR50 anywhere. All
	 * vendor-specific commands are handled the same way for now.
	 */

	/* Verify there is room for at least the extension command header. */
	if (command_size >= sizeof(struct tpm_cmd_header)) {
		uint16_t subcommand_code;

		/* The header takes room in the buffer. */
		*total_size -= sizeof(struct tpm_cmd_header);

		subcommand_code = be16toh(tpmh->subcommand_code);
		rc = tpm_extension_route_command(subcommand_code,
						 tpmh + 1,
						 command_size -
						 sizeof(struct tpm_cmd_header),
						 total_size);
		/* Add the header size back. */
		*total_size += sizeof(struct tpm_cmd_header);
		tpmh->size = htobe32(*total_size);
		/* Flag errors from commands as vendor-specific */
		if (rc)
			rc |= VENDOR_RC_ERR;
		tpmh->command_code = htobe32(rc);
	} else {
		*total_size = command_size;
	}
}
#endif

/* Event (to TPM task) to request reset, or (from TPM task) on completion. */
#define TPM_EVENT_RESET TASK_EVENT_CUSTOM(1 << 0)
#define TPM_EVENT_COMMIT TASK_EVENT_CUSTOM(1 << 1)

/* Calling task (singular) to notify when the TPM reset has completed */
static __initialized task_id_t waiting_for_reset = TASK_ID_INVALID;

/* Return value from blocking tpm_reset_request() call */
static __preserved int wipe_result;

/*
 * Did tpm_reset_request() request nvmem wipe? (intentionally cleared on reset)
 */
static int wipe_requested;

int tpm_reset_request(int wait_until_done, int wipe_nvmem_first)
{
	uint32_t evt;

	cprints(CC_TASK, "%s(%d, %d)", __func__,
		wait_until_done, wipe_nvmem_first);

	if (reset_in_progress) {
		cprints(CC_TASK, "%s: already scheduled", __func__);
		return EC_ERROR_BUSY;
	}

	reset_in_progress = 1;
	wipe_result = EC_SUCCESS;

	/* We can't change our minds about wiping. */
	wipe_requested |= wipe_nvmem_first;

	/* Ask the TPM task to reset itself */
	task_set_event(TASK_ID_TPM, TPM_EVENT_RESET, 0);

	if (!wait_until_done)
		return EC_SUCCESS;

	if (in_interrupt_context() ||
	    task_get_current() == TASK_ID_TPM)
		return EC_ERROR_BUSY;	    /* Can't sleep. Clown'll eat me. */

	/* Completion could take a while, if other things have priority */
	waiting_for_reset = task_get_current();
	evt = task_wait_event_mask(TPM_EVENT_RESET, 5 * SECOND);

	/* We were notified of completion */
	if (evt & TPM_EVENT_RESET)
		return wipe_result;

	/* Timeout is bad */
	return EC_ERROR_TIMEOUT;
}

/*
 * A timeout hook to reinstate NVMEM commits soon after reset.
 *
 * The TPM task disables nvmem commits during TPM reset, they need to be
 * reinstated on the same task context. This is why an event is raised here to
 * wake up the TPM task and force it to reinstate nvmem commits instead of
 * doing it here directly.
 */
static void reinstate_nvmem_commits(void)
{
	tpm_reinstate_nvmem_commits();
}
DECLARE_DEFERRED(reinstate_nvmem_commits);

void tpm_reinstate_nvmem_commits(void)
{
	task_set_event(TASK_ID_TPM, TPM_EVENT_COMMIT, 0);
}

static void tpm_reset_now(int wipe_first)
{
	/* This is more related to TPM task activity than TPM transactions */
	cprints(CC_TASK, "%s(%d)", __func__, wipe_first);

	if (wipe_first) {
		/*
		 * Blindly zapping the TPM space while the AP is awake and
		 * poking at it will bork the TPM task and the AP itself, so
		 * force the whole system off by holding the EC in reset.
		 */
		cprints(CC_TASK, "%s: force EC off", __func__);
		assert_ec_rst();

		/* Now wipe the TPM's nvmem */
		wipe_result = nvmem_erase_user_data(NVMEM_TPM);
	} else {
		wipe_result = EC_SUCCESS;
	}

	/*
	 * Clear the TPM library's zero-init data.  Note that the linker script
	 * includes this file's .bss in the same section, so it will be cleared
	 * at the same time.
	 */
	memset(&__bss_libtpm2_start, 0,
	       (uintptr_t)(&__bss_libtpm2_end) -
	       (uintptr_t)(&__bss_libtpm2_start));

	/*
	 * NOTE: If any __initialized variables need reinitializing after
	 * reset, this is the place to do it.
	 */

	/*
	 * If TPM was reset while commits were disabled, save whatever changes
	 * might have accumulated.
	 */
	nvmem_enable_commits();

	/* Prevent NVRAM commits until further notice. */
	nvmem_disable_commits();

	/* Re-initialize our registers */
	tpm_init();

	if (waiting_for_reset != TASK_ID_INVALID) {
		/* Wake the waiting task, if any */
		task_set_event(waiting_for_reset, TPM_EVENT_RESET, 0);
		waiting_for_reset = TASK_ID_INVALID;
	}

	if (wipe_first) {
		/* Allow AP & EC to boot again */
		cprints(CC_TASK, "%s: allow EC to boot", __func__);
		deassert_ec_rst();
	}

	cprints(CC_TASK, "%s: done", __func__);

	/*
	 * The host might decide to do it sooner, but let's make sure commits
	 * do not stay disabled for more than 3 seconds.
	 */
	hook_call_deferred(&reinstate_nvmem_commits_data, 3 * SECOND);

	reset_in_progress = 0;
}

void tpm_task(void)
{
	uint32_t evt;

	/*
	 * Just in case there is a resume from deep sleep where AP is not out
	 * of reset, let's not proceed until AP is actually up.
	 */
	while (device_get_state(DEVICE_AP) != DEVICE_STATE_ON) {
		/*
		 * The only event we should expect at this point would be the
		 * reset request.
		 */
		evt = task_wait_event(-1);
		if (evt & TPM_EVENT_RESET)
			break;

		cprints(CC_TASK, "%s: unexpected event %x\n", __func__, evt);
	}

	tpm_reset_now(0);
	while (1) {
		uint8_t *response;
		unsigned response_size;
		uint32_t command_code;
		struct tpm_cmd_header *tpmh;

		/* Wait for the next command event */
		evt = task_wait_event(-1);
		if (evt & TPM_EVENT_RESET) {
			tpm_reset_now(wipe_requested);
			/*
			 * There is no point in looking at other events in
			 * this situation: the nvram will be committed by TPM
			 * reset; other tpm commands would be ignored.
			 *
			 * Let's just continue. This could change if there are
			 * other events added to the set.
			 */
			continue;
		}

		if (evt & TPM_EVENT_COMMIT)
			nvmem_enable_commits();

		if (!(evt & TASK_EVENT_WAKE))
			continue;

		tpmh = (struct tpm_cmd_header *)tpm_.regs.data_fifo;
		command_code = be32toh(tpmh->command_code);
		CPRINTF("%s: received fifo command 0x%04x\n",
			__func__, command_code);

		watchdog_reload();

#ifdef CONFIG_EXTENSION_COMMAND
		if (IS_CUSTOM_CODE(command_code)) {
			response_size = sizeof(tpm_.regs.data_fifo);
			call_extension_command(tpmh, &response_size);
		} else
#endif
		{
			ExecuteCommand(tpm_.fifo_write_index,
				       tpm_.regs.data_fifo,
				       &response_size,
				       &response);
		}
		CPRINTF("got %d bytes in response\n", response_size);
		if (response_size &&
		    (response_size <= sizeof(tpm_.regs.data_fifo))) {
			uint32_t tpm_sts;
			/*
			 * TODO(vbendeb): revisit this when
			 * crosbug.com/p/55667 has been addressed.
			 */
			if (command_code == TPM2_PCR_Read)
				system_process_retry_counter();

			else if (command_code == TPM2_Startup)
				read_fwmp();

#ifdef CONFIG_EXTENSION_COMMAND
			if (!IS_CUSTOM_CODE(command_code))
#endif
			{
				/*
				 * Extension commands reuse FIFO buffer, the
				 * rest need to copy.
				 */
				memcpy(tpm_.regs.data_fifo,
				       response, response_size);
			}
			tpm_.fifo_read_index = 0;
			tpm_.fifo_write_index = response_size;
			set_tpm_state(tpm_state_completing_cmd);
			tpm_sts = tpm_.regs.sts;
			tpm_sts &= ~(burst_count_mask << burst_count_shift);
			tpm_sts |= (MIN(response_size, 63) << burst_count_shift)
				| data_avail;
			tpm_.regs.sts = tpm_sts;
		}
	}
}
