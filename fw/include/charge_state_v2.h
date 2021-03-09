/* Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "battery.h"
#include "battery_smart.h"
#include "charger.h"
#include "timer.h"

#ifndef __CROS_EC_CHARGE_STATE_V2_H
#define __CROS_EC_CHARGE_STATE_V2_H

#if defined(CONFIG_I2C_VIRTUAL_BATTERY) && defined(CONFIG_BATTERY_SMART)
#define VIRTUAL_BATTERY_ADDR BATTERY_ADDR
#endif
/*
 * The values exported by charge_get_state() and charge_get_flags() are used
 * only to control the LEDs (with one not-quite-correct exception). For V2
 * we use a different set of states internally.
 */
enum charge_state_v2 {
	ST_IDLE = 0,
	ST_DISCHARGE,
	ST_CHARGE,
	ST_PRECHARGE,

	NUM_STATES_V2
};

struct charge_state_data {
	timestamp_t ts;
	int ac;
	int batt_is_charging;
	struct charger_params chg;
	struct batt_params batt;
	enum charge_state_v2 state;
	int requested_voltage;
	int requested_current;
	int desired_input_current;
};

/**
 * Set the charge input current limit. This value is stored and sent every
 * time AC is applied.
 *
 * @param ma New input current limit in mA
 * @param mv Negotiated charge voltage in mV.
 * @return EC_SUCCESS or error
 */
int charge_set_input_current_limit(int ma, int mv);

#endif /* __CROS_EC_CHARGE_STATE_V2_H */

