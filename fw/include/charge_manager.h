/* Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef __CROS_EC_CHARGE_MANAGER_H
#define __CROS_EC_CHARGE_MANAGER_H

#include "common.h"

/* Charge port that indicates no active port */
#define CHARGE_SUPPLIER_NONE -1
#define CHARGE_PORT_NONE -1
#define CHARGE_CEIL_NONE -1

/* Initial charge state */
#define CHARGE_CURRENT_UNINITIALIZED -1
#define CHARGE_VOLTAGE_UNINITIALIZED -1

/**
 * Time to delay for detecting the charger type (must be long enough for BC1.2
 * driver to get supplier information and notify charge manager).
 */
#define CHARGE_DETECT_DELAY (2*SECOND)

/* Commonly-used charge suppliers listed in no particular order */
enum charge_supplier {
	CHARGE_SUPPLIER_PD,
	CHARGE_SUPPLIER_TYPEC,
	CHARGE_SUPPLIER_BC12_DCP,
	CHARGE_SUPPLIER_BC12_CDP,
	CHARGE_SUPPLIER_BC12_SDP,
	CHARGE_SUPPLIER_PROPRIETARY,
	CHARGE_SUPPLIER_OTHER,
	CHARGE_SUPPLIER_VBUS,
	CHARGE_SUPPLIER_COUNT
};

/* Charge tasks report available current and voltage */
struct charge_port_info {
	int current;
	int voltage;
};

/**
 * Called by charging tasks to update their available charge.
 *
 * @param supplier		Charge supplier to update.
 * @param port			Charge port to update.
 * @param charge		Charge port current / voltage.
 */
void charge_manager_update_charge(int supplier,
				  int port,
				  struct charge_port_info *charge);

/* Partner port dualrole capabilities */
enum dualrole_capabilities {
	CAP_UNKNOWN,
	CAP_DUALROLE,
	CAP_DEDICATED,
};

/**
 * Notify charge_manager of a partner dualrole capability change.
 *
 * @param port			Charge port which changed.
 * @param cap			New port capability.
 */
void charge_manager_update_dualrole(int port, enum dualrole_capabilities cap);

/**
 * Charge ceiling can be set independently by different tasks / functions,
 * for different purposes.
 */
enum ceil_requestor {
	/* Set by PD task, during negotiation */
	CEIL_REQUESTOR_PD,
	/* Set by host commands */
	CEIL_REQUESTOR_HOST,
	/* Number of ceiling groups */
	CEIL_REQUESTOR_COUNT,
};

#define CHARGE_PORT_COUNT \
		(CONFIG_USB_PD_PORT_COUNT + CONFIG_DEDICATED_CHARGE_PORT_COUNT)

/**
 * Update charge ceiling for a given port. The ceiling can be set independently
 * for several requestors, and the min. ceil will be enforced.
 *
 * @param port			Charge port to update.
 * @param requestor		Charge ceiling requestor.
 * @param ceil			Charge ceiling (mA).
 */
void charge_manager_set_ceil(int port, enum ceil_requestor requestor, int ceil);

/*
 * Update PD charge ceiling for a given port. In the event that our ceiling
 * is currently above ceil, change the current limit before returning, without
 * waiting for a charge manager refresh. This function should only be used in
 * time-critical situations where we absolutely cannot proceed without limiting
 * our input current, and it should only be called from the PD tasks.
 * If you ever call this function then you are a terrible person.
 */
void charge_manager_force_ceil(int port, int ceil);

/**
 * Select an 'override port', a port which is always the preferred charge port.
 *
 * @param port			Charge port to select as override, or
 *				OVERRIDE_OFF to select no override port,
 *				or OVERRIDE_DONT_CHARGE to specific that no
 *				charge port should be selected.
 * @return			EC_SUCCESS on success,
 *				the other ec_error_list status on failure.
 */
int charge_manager_set_override(int port);

/**
 * Get the override port.
 *
 * @return	Port number or OVERRIDE_OFF or OVERRIDE_DONT_CHARGE.
 */
int charge_manager_get_override(void);

/**
 * Get the current active charge port, as determined by charge manager.
 *
 * @return	Current active charge port.
 */
int charge_manager_get_active_charge_port(void);

/**
 * Get the power limit set by charge manager.
 *
 * @return	Power limit (uW).
 */
int charge_manager_get_power_limit_uw(void);

/**
 * Get the charger current (mA) value.
 *
 * @return	Charger current (mA) or CHARGE_CURRENT_UNINITIALIZED.
 */
int charge_manager_get_charger_current(void);

#ifdef CONFIG_USB_PD_LOGGING
/* Save power state log entry for the given port */
void charge_manager_save_log(int port);
#endif

/**
 * Update whether a given port is sourcing current.
 *
 * @param port		Port number to be updated.
 * @param enable	0 if the source port is disabled;
 *			Otherwise the source port is enabled.
 */
void charge_manager_source_port(int port, int enable);

/**
 * Get PD source power data objects.
 *
 * @param src_pdo pointer to the data to return.
 * @return number of PDOs returned.
 */
int charge_manager_get_source_pdo(const uint32_t **src_pdo);

/* Board-level callback functions */

/**
 * Set the passed charge port as active.`
 *
 * @param charge_port	Charge port to be enabled.
 * @return		EC_SUCCESS if the charge port is accepted,
 *			other ec_error_list status otherwise.
 */
int board_set_active_charge_port(int charge_port);

/**
 * Set the charge current limit.
 *
 * @param port PD port.
 * @param supplier Identified CHARGE_SUPPLIER_*.
 * @param charge_ma Desired charge current limit, <= max_ma.
 * @param max_ma Maximum charge current limit, >= charge_ma.
 * @param charge_mv Negotiated charge voltage (mV).
 */
void board_set_charge_limit(int port, int supplier, int charge_ma,
			    int max_ma, int charge_mv);

/**
 * Get whether the port is sourcing power on VBUS.
 *
 * @param port PD port.
 * @return VBUS power state.
 */
int board_vbus_source_enabled(int port);

#endif /* __CROS_EC_CHARGE_MANAGER_H */
