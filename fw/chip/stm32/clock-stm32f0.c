/* Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/* Clocks and power management settings */

#include "chipset.h"
#include "clock.h"
#include "clock-f.h"
#include "common.h"
#include "console.h"
#include "cpu.h"
#include "hooks.h"
#include "hwtimer.h"
#include "registers.h"
#include "system.h"
#include "task.h"
#include "timer.h"
#include "util.h"

/* Console output macros */
#define CPUTS(outstr) cputs(CC_CLOCK, outstr)
#define CPRINTS(format, args...) cprints(CC_CLOCK, format, ## args)

/* use 48Mhz USB-synchronized High-speed oscillator */
#define HSI48_CLOCK 48000000

/* use PLL at 38.4MHz as system clock. */
#define PLL_CLOCK 38400000

/* Low power idle statistics */
#ifdef CONFIG_LOW_POWER_IDLE
static int idle_sleep_cnt;
static int idle_dsleep_cnt;
static uint64_t idle_dsleep_time_us;
static int dsleep_recovery_margin_us = 1000000;

/*
 * minimum delay to enter stop mode
 * STOP_MODE_LATENCY: max time to wake up from STOP mode with regulator in low
 * power mode is 5 us + PLL locking time is 200us.
 * SET_RTC_MATCH_DELAY: max time to set RTC match alarm. if we set the alarm
 * in the past, it will never wake up and cause a watchdog.
 * For STM32F3, we are using HSE, which requires additional time to start up.
 * Therefore, the latency for STM32F3 is set longer.
 */
#ifdef CHIP_VARIANT_STM32F373
#define STOP_MODE_LATENCY 500  /* us */
#elif defined(CHIP_VARIANT_STM32F05X)
#define STOP_MODE_LATENCY 300  /* us */
#elif (CPU_CLOCK == PLL_CLOCK)
#define STOP_MODE_LATENCY 300   /* us */
#else
#define STOP_MODE_LATENCY 50    /* us */
#endif
#define SET_RTC_MATCH_DELAY 200 /* us */

#endif /* CONFIG_LOW_POWER_IDLE */

/*
 * RTC clock frequency (connected to LSI clock)
 *
 * TODO(crosbug.com/p/12281): Calibrate LSI frequency on a per-chip basis.  The
 * LSI on any given chip can be between 30 kHz to 60 kHz.  Without calibration,
 * LSI frequency may be off by as much as 50%.  Fortunately, we don't do any
 * high-precision delays based solely on LSI.
 */
/*
 * Set synchronous clock freq to LSI/2 (20kHz) to maximize subsecond
 * resolution. Set asynchronous clock to 1 Hz.
 */
#define RTC_FREQ (40000 / 2) /* Hz */
#define RTC_PREDIV_S (RTC_FREQ - 1)
#define RTC_PREDIV_A 1
#define US_PER_RTC_TICK (1000000 / RTC_FREQ)


int32_t rtcss_to_us(uint32_t rtcss)
{
	return ((RTC_PREDIV_S - rtcss) * US_PER_RTC_TICK);
}

uint32_t us_to_rtcss(int32_t us)
{
	return (RTC_PREDIV_S - (us / US_PER_RTC_TICK));
}


void config_hispeed_clock(void)
{
#ifdef CHIP_FAMILY_STM32F3
	/* Ensure that HSE is ON */
	if (!(STM32_RCC_CR & (1 << 17))) {
		/* Enable HSE */
		STM32_RCC_CR |= 1 << 16;
		/* Wait for HSE to be ready */
		while (!(STM32_RCC_CR & (1 << 17)))
			;
	}

	/*
	 * HSE = 24MHz, no prescalar, no MCO, with PLL *2 => 48MHz SYSCLK
	 * HCLK = SYSCLK, PCLK = HCLK / 2 = 24MHz
	 * ADCCLK = PCLK / 6 = 4MHz
	 * USB uses SYSCLK = 48MHz
	 */
	STM32_RCC_CFGR = 0x0041a400;

	/* Enable the PLL */
	STM32_RCC_CR |= 0x01000000;

	/* Wait until the PLL is ready */
	while (!(STM32_RCC_CR & 0x02000000))
		;

	/* Switch SYSCLK to PLL */
	STM32_RCC_CFGR |= 0x2;

	/* Wait until the PLL is the clock source */
	while ((STM32_RCC_CFGR & 0xc) != 0x8)
		;
/* F03X and F05X and F070 don't have HSI48 */
#elif defined(CHIP_VARIANT_STM32F03X) || \
defined(CHIP_VARIANT_STM32F05X) || \
defined(CHIP_VARIANT_STM32F070)
	/* If PLL is the clock source, PLL has already been set up. */
	if ((STM32_RCC_CFGR & 0xc) == 0x8)
		return;

	/* Ensure that HSI is ON */
	if (!(STM32_RCC_CR & (1<<1))) {
		/* Enable HSI */
		STM32_RCC_CR |= (1<<0);
		/* Wait for HSI to be ready */
		while (!(STM32_RCC_CR & (1<<1)))
			;
	}

	/*
	 * HSI = 8MHz, HSI/2 with PLL *12 = ~48 MHz
	 * therefore PCLK = FCLK = SYSCLK = 48MHz
	 */
	/* Switch the PLL source to HSI/2 */
	STM32_RCC_CFGR &= ~(0x00018000);

	/*
	 * Specify HSI/2 clock as input clock to PLL and set PLL (*12).
	 */
	STM32_RCC_CFGR |= 0x00280000;

	/* Enable the PLL. */
	STM32_RCC_CR |= 0x01000000;

	/* Wait until PLL is ready. */
	while (!(STM32_RCC_CR & 0x02000000))
		;

	/* Switch SYSCLK to PLL. */
	STM32_RCC_CFGR |= 0x2;

	/* wait until the PLL is the clock source */
	while ((STM32_RCC_CFGR & 0xc) != 0x8)
		;
#else
	/* Ensure that HSI48 is ON */
	if (!(STM32_RCC_CR2 & (1 << 17))) {
		/* Enable HSI */
		STM32_RCC_CR2 |= 1 << 16;
		/* Wait for HSI to be ready */
		while (!(STM32_RCC_CR2 & (1 << 17)))
			;
	}

#if (CPU_CLOCK == HSI48_CLOCK)
	/*
	 * HSI48 = 48MHz, no prescaler, no MCO, no PLL
	 * therefore PCLK = FCLK = SYSCLK = 48MHz
	 * USB uses HSI48 = 48MHz
	 */

#ifdef CONFIG_USB
	/*
	 * Configure and enable Clock Recovery System
	 *
	 * Since we are running from the internal RC HSI48 clock, the CSR
	 * is needed to guarantee an accurate 48MHz clock for USB.
	 *
	 * The default values configure the CRS to use the periodic USB SOF
	 * as the SYNC signal for calibrating the HSI48.
	 *
	 */

	/* Enable Clock Recovery System */
	STM32_RCC_APB1ENR |= STM32_RCC_PB1_CRS;

	/* Enable automatic trimming */
	STM32_CRS_CR |= STM32_CRS_CR_AUTOTRIMEN;

	/* Enable oscillator clock for the frequency error counter */
	STM32_CRS_CR |= STM32_CRS_CR_CEN;
#endif

	/* switch SYSCLK to HSI48 */
	STM32_RCC_CFGR = 0x00000003;

	/* wait until the HSI48 is the clock source */
	while ((STM32_RCC_CFGR & 0xc) != 0xc)
		;

#elif (CPU_CLOCK == PLL_CLOCK)
	/*
	 * HSI48 = 48MHz, no prescalar, no MCO, with PLL *4/5 => 38.4MHz SYSCLK
	 * therefore PCLK = FCLK = SYSCLK = 38.4MHz
	 * USB uses HSI48 = 48MHz
	 */

	/* If PLL is the clock source, PLL has already been set up. */
	if ((STM32_RCC_CFGR & 0xc) == 0x8)
		return;

	/*
	 * Specify HSI48 clock as input clock to PLL and set PLL multiplier
	 * and divider.
	 */
	STM32_RCC_CFGR = 0x00098000;
	STM32_RCC_CFGR2 = 0x4;

	/* Enable the PLL. */
	STM32_RCC_CR |= 0x01000000;

	/* Wait until PLL is ready. */
	while (!(STM32_RCC_CR & 0x02000000))
		;

	/* Switch SYSCLK to PLL. */
	STM32_RCC_CFGR |= 0x2;

	/* wait until the PLL is the clock source */
	while ((STM32_RCC_CFGR & 0xc) != 0x8)
		;

#else
#error "CPU_CLOCK must be either 48MHz or 38.4MHz"
#endif
#endif
}

#ifdef CONFIG_HIBERNATE
void __enter_hibernate(uint32_t seconds, uint32_t microseconds)
{
	uint32_t rtc, rtcss;

	if (seconds || microseconds)
		set_rtc_alarm(seconds, microseconds, &rtc, &rtcss);

	/* interrupts off now */
	asm volatile("cpsid i");

#ifdef CONFIG_HIBERNATE_WAKEUP_PINS
	/* enable the wake up pins */
	STM32_PWR_CSR |= CONFIG_HIBERNATE_WAKEUP_PINS;
#endif
	STM32_PWR_CR |= 0xe;
	CPU_SCB_SYSCTRL |= 0x4;
	/* go to Standby mode */
	asm("wfi");

	/* we should never reach that point */
	while (1)
		;
}
#endif

#ifdef CONFIG_LOW_POWER_IDLE

void clock_refresh_console_in_use(void)
{
}

#ifdef CONFIG_FORCE_CONSOLE_RESUME
#define UARTN_BASE STM32_USART_BASE(CONFIG_UART_CONSOLE)
static void enable_serial_wakeup(int enable)
{
	if (enable) {
		/*
		 * Allow UART wake up from STOP mode. Note, UART clock must
		 * be HSI(8MHz) for wakeup to work.
		 */
		STM32_USART_CR1(UARTN_BASE) |= STM32_USART_CR1_UESM;
		STM32_USART_CR3(UARTN_BASE) |= STM32_USART_CR3_WUFIE;
	} else {
		/* Disable wake up from STOP mode. */
		STM32_USART_CR1(UARTN_BASE) &= ~STM32_USART_CR1_UESM;
	}
}
#else
static void enable_serial_wakeup(int enable)
{
}
#endif

void __idle(void)
{
	timestamp_t t0;
	int next_delay, margin_us, rtc_diff;
	uint32_t rtc0, rtc0ss, rtc1, rtc1ss;

	while (1) {
		asm volatile("cpsid i");

		t0 = get_time();
		next_delay = __hw_clock_event_get() - t0.le.lo;

		if (DEEP_SLEEP_ALLOWED &&
		    (next_delay > (STOP_MODE_LATENCY + SET_RTC_MATCH_DELAY))) {
			/* deep-sleep in STOP mode */
			idle_dsleep_cnt++;

			enable_serial_wakeup(1);

			/* set deep sleep bit */
			CPU_SCB_SYSCTRL |= 0x4;

			set_rtc_alarm(0, next_delay - STOP_MODE_LATENCY,
				      &rtc0, &rtc0ss);
			asm("wfi");

			CPU_SCB_SYSCTRL &= ~0x4;

			enable_serial_wakeup(0);

			/*
			 * By default only HSI 8MHz is enabled here. Re-enable
			 * high-speed clock if in use.
			 */
			config_hispeed_clock();

			/* fast forward timer according to RTC counter */
			reset_rtc_alarm(&rtc1, &rtc1ss);
			rtc_diff = get_rtc_diff(rtc0, rtc0ss, rtc1, rtc1ss);
			t0.val = t0.val + rtc_diff;
			force_time(t0);

			/* Record time spent in deep sleep. */
			idle_dsleep_time_us += rtc_diff;

			/* Calculate how close we were to missing deadline */
			margin_us = next_delay - rtc_diff;
			if (margin_us < 0)
				/* Use CPUTS to save stack space */
				CPUTS("Idle overslept!\n");

			/* Record the closest to missing a deadline. */
			if (margin_us < dsleep_recovery_margin_us)
				dsleep_recovery_margin_us = margin_us;
		} else {
			idle_sleep_cnt++;

			/* normal idle : only CPU clock stopped */
			asm("wfi");
		}
		asm volatile("cpsie i");
	}
}
#endif /* CONFIG_LOW_POWER_IDLE */

int clock_get_freq(void)
{
	return CPU_CLOCK;
}

void clock_wait_bus_cycles(enum bus_type bus, uint32_t cycles)
{
	volatile uint32_t dummy __attribute__((unused));

	if (bus == BUS_AHB) {
		while (cycles--)
			dummy = STM32_DMA1_REGS->isr;
	} else { /* APB */
		while (cycles--)
			dummy = STM32_USART_BRR(STM32_USART1_BASE);
	}
}

void clock_enable_module(enum module_id module, int enable)
{
}

void rtc_init(void)
{
	rtc_unlock_regs();

	/* Enter RTC initialize mode */
	STM32_RTC_ISR |= STM32_RTC_ISR_INIT;
	while (!(STM32_RTC_ISR & STM32_RTC_ISR_INITF))
		;

	/* Set clock prescalars */
	STM32_RTC_PRER = (RTC_PREDIV_A << 16) | RTC_PREDIV_S;

	/* Start RTC timer */
	STM32_RTC_ISR &= ~STM32_RTC_ISR_INIT;
	while (STM32_RTC_ISR & STM32_RTC_ISR_INITF)
		;

	/* Enable RTC alarm interrupt */
	STM32_RTC_CR |= STM32_RTC_CR_ALRAIE | STM32_RTC_CR_BYPSHAD;
	STM32_EXTI_RTSR |= EXTI_RTC_ALR_EVENT;
	task_enable_irq(STM32_IRQ_RTC_ALARM);

	rtc_lock_regs();
}


#if defined(CONFIG_LOW_POWER_IDLE) && defined(CONFIG_COMMON_RUNTIME)
#ifdef CONFIG_CMD_IDLE_STATS
/**
 * Print low power idle statistics
 */
static int command_idle_stats(int argc, char **argv)
{
	timestamp_t ts = get_time();

	ccprintf("Num idle calls that sleep:           %d\n", idle_sleep_cnt);
	ccprintf("Num idle calls that deep-sleep:      %d\n", idle_dsleep_cnt);
	ccprintf("Time spent in deep-sleep:            %.6lds\n",
			idle_dsleep_time_us);
	ccprintf("Total time on:                       %.6lds\n", ts.val);
	ccprintf("Deep-sleep closest to wake deadline: %dus\n",
			dsleep_recovery_margin_us);

	return EC_SUCCESS;
}
DECLARE_CONSOLE_COMMAND(idlestats, command_idle_stats,
			"",
			"Print last idle stats");
#endif /* CONFIG_CMD_IDLE_STATS */
#endif
