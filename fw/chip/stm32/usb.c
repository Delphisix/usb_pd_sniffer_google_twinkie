/* Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "clock.h"
#include "common.h"
#include "config.h"
#include "console.h"
#include "flash.h"
#include "gpio.h"
#include "hooks.h"
#include "link_defs.h"
#include "registers.h"
#include "system.h"
#include "task.h"
#include "timer.h"
#include "util.h"
#include "usb_api.h"
#include "usb_descriptor.h"
#include "usb_hw.h"

/* Console output macro */
#define CPRINTF(format, args...) cprintf(CC_USB, format, ## args)

#ifdef CONFIG_USB_BOS
/* v2.10 (vs 2.00) BOS Descriptor provided */
#define USB_DEV_BCDUSB 0x0210
#else
#define USB_DEV_BCDUSB 0x0200
#endif

#ifndef USB_DEV_CLASS
#define USB_DEV_CLASS USB_CLASS_PER_INTERFACE
#endif

#ifndef CONFIG_USB_BCD_DEV
#define CONFIG_USB_BCD_DEV 0x0100 /* 1.00 */
#endif

#ifndef CONFIG_USB_SERIALNO
#define USB_STR_SERIALNO 0
#else
static int usb_load_serial(void);
#endif

#define USB_RESUME_TIMEOUT_MS 300

/* USB Standard Device Descriptor */
static const struct usb_device_descriptor dev_desc = {
	.bLength = USB_DT_DEVICE_SIZE,
	.bDescriptorType = USB_DT_DEVICE,
	.bcdUSB = USB_DEV_BCDUSB,
	.bDeviceClass = USB_DEV_CLASS,
	.bDeviceSubClass = 0x00,
	.bDeviceProtocol = 0x00,
	.bMaxPacketSize0 = USB_MAX_PACKET_SIZE,
	.idVendor = USB_VID_GOOGLE,
	.idProduct = CONFIG_USB_PID,
	.bcdDevice = CONFIG_USB_BCD_DEV,
	.iManufacturer = USB_STR_VENDOR,
	.iProduct = USB_STR_PRODUCT,
	.iSerialNumber = USB_STR_SERIALNO,
	.bNumConfigurations = 1
};

/* USB Configuration Descriptor */
const struct usb_config_descriptor USB_CONF_DESC(conf) = {
	.bLength = USB_DT_CONFIG_SIZE,
	.bDescriptorType = USB_DT_CONFIGURATION,
	.wTotalLength = 0x0BAD, /* no of returned bytes, set at runtime */
	.bNumInterfaces = USB_IFACE_COUNT,
	.bConfigurationValue = 1,
	.iConfiguration = USB_STR_VERSION,
	.bmAttributes = 0x80 /* Reserved bit */
#ifdef CONFIG_USB_SELF_POWERED  /* bus or self powered */
		      | 0x40
#endif
#ifdef CONFIG_USB_REMOTE_WAKEUP
		      | 0x20
#endif
	,
	.bMaxPower = (CONFIG_USB_MAXPOWER_MA / 2),
};

const uint8_t usb_string_desc[] = {
	4, /* Descriptor size */
	USB_DT_STRING,
	0x09, 0x04 /* LangID = 0x0409: U.S. English */
};

/* Endpoint table in USB controller RAM */
struct stm32_endpoint btable_ep[USB_EP_COUNT] __aligned(8) __usb_btable;
/* Control endpoint (EP0) buffers */
static usb_uint ep0_buf_tx[USB_MAX_PACKET_SIZE / 2] __usb_ram;
static usb_uint ep0_buf_rx[USB_MAX_PACKET_SIZE / 2] __usb_ram;

#define EP0_BUF_TX_SRAM_ADDR ((void *) usb_sram_addr(ep0_buf_tx))

static int set_addr;
/* remaining size of descriptor data to transfer */
static int desc_left;
/* pointer to descriptor data if any */
static const uint8_t *desc_ptr;
/* interface that should handle the next tx transaction */
static uint8_t iface_next = USB_IFACE_COUNT;
#ifdef CONFIG_USB_REMOTE_WAKEUP
/* remote wake up feature enabled */
static int remote_wakeup_enabled;
#endif

void usb_read_setup_packet(usb_uint *buffer, struct usb_setup_packet *packet)
{
	packet->bmRequestType = buffer[0] & 0xff;
	packet->bRequest      = buffer[0] >> 8;
	packet->wValue        = buffer[1];
	packet->wIndex        = buffer[2];
	packet->wLength       = buffer[3];
}

static void ep0_send_descriptor(const uint8_t *desc, int len,
				uint16_t fixup_size)
{
	/* do not send more than what the host asked for */
	len = MIN(ep0_buf_rx[3], len);
	/*
	 * if we cannot transmit everything at once,
	 * keep the remainder for the next IN packet
	 */
	if (len >= USB_MAX_PACKET_SIZE) {
		desc_left = len - USB_MAX_PACKET_SIZE;
		desc_ptr = desc + USB_MAX_PACKET_SIZE;
		len = USB_MAX_PACKET_SIZE;
	}
	memcpy_to_usbram(EP0_BUF_TX_SRAM_ADDR, desc, len);
	if (fixup_size) /* set the real descriptor size */
		ep0_buf_tx[1] = fixup_size;
	btable_ep[0].tx_count = len;
	/* send the null OUT transaction if the transfer is complete */
	STM32_TOGGLE_EP(0, EP_TX_RX_MASK, EP_TX_RX_VALID,
			desc_left ? 0 : EP_STATUS_OUT);
}

/* Requests on the control endpoint (aka EP0) */
static void ep0_rx(void)
{
	uint16_t req = ep0_buf_rx[0]; /* bRequestType | bRequest */

	/* reset any incomplete descriptor transfer */
	desc_ptr = NULL;
	iface_next = USB_IFACE_COUNT;

	/* interface specific requests */
	if ((req & USB_RECIP_MASK) == USB_RECIP_INTERFACE) {
		uint8_t iface = ep0_buf_rx[2] & 0xff;
		if (iface < USB_IFACE_COUNT) {
			int ret;

			ret = usb_iface_request[iface](ep0_buf_rx, ep0_buf_tx);
			if (ret < 0)
				goto unknown_req;
			if (ret == 1)
				iface_next = iface;
			return;
		}
	}
	/* vendor specific request */
	if ((req & USB_TYPE_MASK) == USB_TYPE_VENDOR) {
#ifdef CONFIG_WEBUSB_URL
		uint8_t b_req = req >> 8; /* bRequest in the transfer */
		uint16_t idx = ep0_buf_rx[2]; /* wIndex in the transfer */

		if (b_req == 0x01 && idx == WEBUSB_REQ_GET_URL) {
			int len = *(uint8_t *)webusb_url;

			ep0_send_descriptor(webusb_url, len, 0);
			return;
		}
#endif
		goto unknown_req;
	}

	/* TODO check setup bit ? */
	if (req == (USB_DIR_IN | (USB_REQ_GET_DESCRIPTOR << 8))) {
		uint8_t type = ep0_buf_rx[1] >> 8;
		uint8_t idx = ep0_buf_rx[1] & 0xff;
		const uint8_t *desc;
		int len;

		switch (type) {
		case USB_DT_DEVICE: /* Setup : Get device descriptor */
			desc = (void *)&dev_desc;
			len = sizeof(dev_desc);
			break;
		case USB_DT_CONFIGURATION: /* Setup : Get configuration desc */
			desc = __usb_desc;
			len = USB_DESC_SIZE;
			break;
#ifdef CONFIG_USB_BOS
		case USB_DT_BOS: /* Setup : Get BOS descriptor */
			desc = bos_ctx.descp;
			len = bos_ctx.size;
			break;
#endif
		case USB_DT_STRING: /* Setup : Get string descriptor */
			if (idx >= USB_STR_COUNT)
				/* The string does not exist : STALL */
				goto unknown_req;
#ifdef CONFIG_USB_SERIALNO
			if (idx == USB_STR_SERIALNO)
				desc = (uint8_t *)usb_serialno_desc;
			else
#endif
				desc = usb_strings[idx];
			len = desc[0];
			break;
		case USB_DT_DEVICE_QUALIFIER: /* Get device qualifier desc */
			/* Not high speed : STALL next IN used as handshake */
			goto unknown_req;
		default: /* unhandled descriptor */
			goto unknown_req;
		}
		ep0_send_descriptor(desc, len, type == USB_DT_CONFIGURATION ?
						USB_DESC_SIZE : 0);
	} else if (req == (USB_DIR_IN | (USB_REQ_GET_STATUS << 8))) {
		uint16_t data = 0;
		/* Get status */
#ifdef CONFIG_USB_SELF_POWERED
		data |= USB_REQ_GET_STATUS_SELF_POWERED;
#endif
#ifdef CONFIG_USB_REMOTE_WAKEUP
		if (remote_wakeup_enabled)
			data |= USB_REQ_GET_STATUS_REMOTE_WAKEUP;
#endif
		memcpy_to_usbram(EP0_BUF_TX_SRAM_ADDR, (void *)&data, 2);
		btable_ep[0].tx_count = 2;
		STM32_TOGGLE_EP(0, EP_TX_RX_MASK, EP_TX_RX_VALID,
			  EP_STATUS_OUT /*null OUT transaction */);
	} else if ((req & 0xff) == USB_DIR_OUT) {
		switch (req >> 8) {
		case USB_REQ_SET_FEATURE:
		case USB_REQ_CLEAR_FEATURE:
#ifdef CONFIG_USB_REMOTE_WAKEUP
			if (ep0_buf_rx[1] ==
					USB_REQ_FEATURE_DEVICE_REMOTE_WAKEUP) {
				remote_wakeup_enabled =
					((req >> 8) == USB_REQ_SET_FEATURE);
				btable_ep[0].tx_count = 0;
				STM32_TOGGLE_EP(0, EP_TX_RX_MASK,
						EP_TX_RX_VALID, 0);
				break;
			}
#endif
			goto unknown_req;
		case USB_REQ_SET_ADDRESS:
			/* set the address after we got IN packet handshake */
			set_addr = ep0_buf_rx[1] & 0xff;
			/* need null IN transaction -> TX Valid */
			btable_ep[0].tx_count = 0;
			STM32_TOGGLE_EP(0, EP_TX_RX_MASK, EP_TX_RX_VALID, 0);
			break;
		case USB_REQ_SET_CONFIGURATION:
			/* uint8_t cfg = ep0_buf_rx[1] & 0xff; */
			/* null IN for handshake */
			btable_ep[0].tx_count = 0;
			STM32_TOGGLE_EP(0, EP_TX_RX_MASK, EP_TX_RX_VALID, 0);
			break;
		default: /* unhandled request */
			goto unknown_req;
		}

	} else {
		goto unknown_req;
	}

	return;
unknown_req:
	STM32_TOGGLE_EP(0, EP_TX_RX_MASK, EP_RX_VALID | EP_TX_STALL, 0);
}

static void ep0_tx(void)
{
	if (set_addr) {
		STM32_USB_DADDR = set_addr | 0x80;
		set_addr = 0;
		CPRINTF("SETAD %02x\n", STM32_USB_DADDR);
	}
	if (desc_ptr) {
		/* we have an on-going descriptor transfer */
		int len = MIN(desc_left, USB_MAX_PACKET_SIZE);
		memcpy_to_usbram(EP0_BUF_TX_SRAM_ADDR, desc_ptr, len);
		btable_ep[0].tx_count = len;
		desc_left -= len;
		desc_ptr += len;
		STM32_TOGGLE_EP(0, EP_TX_MASK, EP_TX_VALID,
				desc_left ? 0 : EP_STATUS_OUT);
		/* send the null OUT transaction if the transfer is complete */
		return;
	}
	if (iface_next < USB_IFACE_COUNT) {
		int ret;

		ret = usb_iface_request[iface_next](NULL, ep0_buf_tx);
		if (ret < 0)
			goto error;
		if (ret == 0)
			iface_next = USB_IFACE_COUNT;
		return;
	}

error:
	STM32_TOGGLE_EP(0, EP_TX_MASK, EP_TX_VALID, 0);
}

static void ep0_event(enum usb_ep_event evt)
{
	if (evt != USB_EVENT_RESET)
		return;

	STM32_USB_EP(0) = (1 << 9) /* control EP */ |
			  (2 << 4) /* TX NAK */ |
			  (3 << 12) /* RX VALID */;

	btable_ep[0].tx_addr = usb_sram_addr(ep0_buf_tx);
	btable_ep[0].rx_addr = usb_sram_addr(ep0_buf_rx);
	btable_ep[0].rx_count = 0x8000 | ((USB_MAX_PACKET_SIZE/32-1) << 10);
	btable_ep[0].tx_count = 0;
}
USB_DECLARE_EP(0, ep0_tx, ep0_rx, ep0_event);

static void usb_reset(void)
{
	int ep;

	for (ep = 0; ep < USB_EP_COUNT; ep++)
		usb_ep_event[ep](USB_EVENT_RESET);

	/*
	 * set the default address : 0
	 * as we are not configured yet
	 */
	STM32_USB_DADDR = 0 | 0x80;
	CPRINTF("RST EP0 %04x\n", STM32_USB_EP(0));
}

#ifdef CONFIG_USB_SUSPEND
/* See RM0091 Reference Manual 30.5.5 Suspend/Resume events */
static void usb_suspend(void)
{
	CPRINTF("USB suspend!\n");

	/* Set FSUSP bit to activate suspend mode */
	STM32_USB_CNTR |= STM32_USB_CNTR_FSUSP;

	/* Set USB low power mode */
	STM32_USB_CNTR |= STM32_USB_CNTR_LP_MODE;

	clock_enable_module(MODULE_USB, 0);

	/* USB is not in use anymore, we can (hopefully) sleep now. */
	enable_sleep(SLEEP_MASK_USB_DEVICE);
}

static void usb_resume(void)
{
	int state = (STM32_USB_FNR & STM32_USB_FNR_RXDP_RXDM_MASK)
			>> STM32_USB_FNR_RXDP_RXDM_SHIFT;

	CPRINTF("USB resume %x\n", state);

	/*
	 * TODO(crosbug.com/p/63273): Reference manual suggests going back to
	 * sleep if state is 10 or 11, but this seems to cause other problems
	 * (see bug). Ignore them for now.
	 */

	clock_enable_module(MODULE_USB, 1);

	/* Clear FSUSP bit to exit suspend mode */
	STM32_USB_CNTR &= ~STM32_USB_CNTR_FSUSP;

	/* USB is in use again */
	disable_sleep(SLEEP_MASK_USB_DEVICE);
}

#ifdef CONFIG_USB_REMOTE_WAKEUP
/*
 * Makes sure usb_wake is only run once. When 0, wake is in progress.
 */
static volatile int usb_wake_done = 1;

/*
 * ESOF counter (incremented in interrupt), RESUME bit is cleared when
 * this reaches 0. Also used to detect resume timeout.
 */
static volatile int esof_count;

__attribute__((weak))
void board_usb_wake(void)
{
	/* Side-band USB wake, do nothing by default. */
}

void usb_wake(void)
{
	if (!remote_wakeup_enabled ||
	    !(STM32_USB_CNTR & STM32_USB_CNTR_FSUSP)) {
		/*
		 * USB wake not enabled, or already woken up, or already waking
		 * up,nothing to do.
		 */
		return;
	}

	/* Only allow one caller at a time. */
	if (!atomic_read_clear(&usb_wake_done))
		return;

	CPRINTF("USB wake\n");

	/*
	 * Set RESUME bit for 1 to 15 ms, then clear it. We ask the interrupt
	 * routine to count 3 ESOF interrupts, which should take between
	 * 2 and 3 ms.
	 */
	esof_count = 3;
	STM32_USB_CNTR |= STM32_USB_CNTR_RESUME | STM32_USB_CNTR_ESOFM;

	/* Try side-band wake as well. */
	board_usb_wake();
}
#endif

int usb_is_suspended(void)
{
	/* Either hardware block is suspended... */
	if (STM32_USB_CNTR & STM32_USB_CNTR_FSUSP)
		return 1;

#ifdef CONFIG_USB_REMOTE_WAKEUP
	/* ... or we are currently waking up. */
	if (!usb_wake_done)
		return 1;
#endif

	return 0;
}
#endif /* CONFIG_USB_SUSPEND */

void usb_interrupt(void)
{
	uint16_t status = STM32_USB_ISTR;

	if (status & STM32_USB_ISTR_RESET)
		usb_reset();

#ifdef CONFIG_USB_SUSPEND
#ifdef CONFIG_USB_REMOTE_WAKEUP
	/*
	 * usb_wake is asking us to count esof_count ESOF interrupts (one
	 * per millisecond), then disable RESUME, then wait for resume to
	 * complete.
	 */
	if (status & STM32_USB_ISTR_ESOF && !usb_wake_done) {
		esof_count--;

		/* Clear RESUME bit. */
		if (esof_count == 0)
			STM32_USB_CNTR &= ~STM32_USB_CNTR_RESUME;

		/* Then count down until state is resumed. */
		if (esof_count <= 0) {
			int state;

			state = (STM32_USB_FNR & STM32_USB_FNR_RXDP_RXDM_MASK)
					>> STM32_USB_FNR_RXDP_RXDM_SHIFT;

			/* Either: state is ready, or we timed out. */
			if (state == 2 || state == 3 ||
			    esof_count <= -USB_RESUME_TIMEOUT_MS) {
				STM32_USB_CNTR &= ~STM32_USB_CNTR_ESOFM;
				usb_wake_done = 1;
				if (state != 2) {
					CPRINTF("wake error: cnt=%d state=%d\n",
						esof_count, state);
					usb_suspend();
				}
			}
		}
	}
#endif

	if (status & STM32_USB_ISTR_SUSP)
		usb_suspend();

	if (status & STM32_USB_ISTR_WKUP)
		usb_resume();
#endif

	if (status & STM32_USB_ISTR_CTR) {
		int ep = status & STM32_USB_ISTR_EP_ID_MASK;
		if (ep < USB_EP_COUNT) {
			if (status & STM32_USB_ISTR_DIR)
				usb_ep_rx[ep]();
			else
				usb_ep_tx[ep]();
		}
		/* TODO: do it in a USB task */
		/* task_set_event(, 1 << ep_task); */
	}

	/* ack only interrupts that we handled */
	STM32_USB_ISTR = ~status;
}
DECLARE_IRQ(STM32_IRQ_USB_LP, usb_interrupt, 1);

void usb_init(void)
{
	/* Enable USB device clock. */
	STM32_RCC_APB1ENR |= STM32_RCC_PB1_USB;

	/* we need a proper 48MHz clock */
	clock_enable_module(MODULE_USB, 1);

	/* configure the pinmux */
	gpio_config_module(MODULE_USB, 1);

	/* power on sequence */

	/* keep FRES (USB reset) and remove PDWN (power down) */
	STM32_USB_CNTR = STM32_USB_CNTR_FRES;
	udelay(1); /* startup time */
	/* reset FRES and keep interrupts masked */
	STM32_USB_CNTR = 0x00;
	/* clear pending interrupts */
	STM32_USB_ISTR = 0;

	/* set descriptors table offset in dedicated SRAM */
	STM32_USB_BTABLE = 0;

	/* EXTI18 is USB wake up interrupt */
	/* STM32_EXTI_RTSR |= 1 << 18; */
	/* STM32_EXTI_IMR |= 1 << 18; */

	/* Enable interrupt handlers */
	task_enable_irq(STM32_IRQ_USB_LP);
	/* set interrupts mask : reset/correct transfer/errors */
	STM32_USB_CNTR = STM32_USB_CNTR_CTRM |
			 STM32_USB_CNTR_PMAOVRM |
			 STM32_USB_CNTR_ERRM |
#ifdef CONFIG_USB_SUSPEND
			 STM32_USB_CNTR_WKUPM |
			 STM32_USB_CNTR_SUSPM |
#endif
			 STM32_USB_CNTR_RESETM;

#ifdef CONFIG_USB_SERIALNO
	usb_load_serial();
#endif
#ifndef CONFIG_USB_INHIBIT_CONNECT
	usb_connect();
#endif

	CPRINTF("USB init done\n");
}

#ifndef CONFIG_USB_INHIBIT_INIT
DECLARE_HOOK(HOOK_INIT, usb_init, HOOK_PRIO_DEFAULT);
#endif

void usb_release(void)
{
	/* signal disconnect to host */
	usb_disconnect();

	/* power down USB */
	STM32_USB_CNTR = 0;

	/* disable interrupt handlers */
	task_disable_irq(STM32_IRQ_USB_LP);

	/* unset pinmux */
	gpio_config_module(MODULE_USB, 0);

	/* disable 48MHz clock */
	clock_enable_module(MODULE_USB, 0);

	/* disable USB device clock */
	STM32_RCC_APB1ENR &= ~STM32_RCC_PB1_USB;
}
/* ensure the host disconnects and reconnects over a sysjump */
DECLARE_HOOK(HOOK_SYSJUMP, usb_release, HOOK_PRIO_DEFAULT);

int usb_is_enabled(void)
{
	return (STM32_RCC_APB1ENR & STM32_RCC_PB1_USB) ? 1 : 0;
}

void *memcpy_to_usbram(void *dest, const void *src, size_t n)
{
	int       unaligned =                 (((uintptr_t) dest) & 1);
	usb_uint *d         = &__usb_ram_start[((uintptr_t) dest) / 2];
	uint8_t  *s         = (uint8_t *) src;
	int       i;

	/*
	 * Handle unaligned leading byte via read/modify/write.
	 */
	if (unaligned && n) {
		*d = (*d & ~0xff00) | (*s << 8);
		n--;
		s++;
		d++;
	}

	for (i = 0; i < n / 2; i++, s += 2)
		*d++ = (s[1] << 8) | s[0];

	/*
	 * There is a trailing byte to write into a final USB packet memory
	 * location, use a read/modify/write to be safe.
	 */
	if (n & 1)
		*d = (*d & ~0x00ff) | *s;

	return dest;
}

void *memcpy_from_usbram(void *dest, const void *src, size_t n)
{
	int             unaligned =                 (((uintptr_t) src) & 1);
	usb_uint const *s         = &__usb_ram_start[((uintptr_t) src) / 2];
	uint8_t        *d         = (uint8_t *) dest;
	int             i;

	if (unaligned && n) {
		*d = *s >> 8;
		n--;
		s++;
		d++;
	}

	for (i = 0; i < n / 2; i++) {
		usb_uint value = *s++;

		*d++ = (value >> 0) & 0xff;
		*d++ = (value >> 8) & 0xff;
	}

	if (n & 1)
		*d = *s;

	return dest;
}

#ifdef CONFIG_USB_SERIALNO
/* This will be subbed into USB_STR_SERIALNO. */
struct usb_string_desc *usb_serialno_desc =
	USB_WR_STRING_DESC(DEFAULT_SERIALNO);

/* Update serial number */
static int usb_set_serial(const char *serialno)
{
	struct usb_string_desc *sd = usb_serialno_desc;
	int i;

	if (!serialno)
		return EC_ERROR_INVAL;

	/* Convert into unicode usb string desc. */
	for (i = 0; i < USB_STRING_LEN; i++) {
		sd->_data[i] = serialno[i];
		if (serialno[i] == 0)
			break;
	}
	/* Count wchars (w/o null terminator) plus size & type bytes. */
	sd->_len = (i * 2) + 2;
	sd->_type = USB_DT_STRING;

	return EC_SUCCESS;
}

/* By default, read serial number from flash, can be overridden. */
__attribute__((weak))
const char *board_read_serial(void)
{
	return flash_read_serial();
}

/* Retrieve serial number from pstate flash. */
static int usb_load_serial(void)
{
	const char *serialno;
	int rv;

	serialno = board_read_serial();
	if (!serialno)
		return EC_ERROR_ACCESS_DENIED;

	rv = usb_set_serial(serialno);
	return rv;
}

/* Save serial number into pstate region. */
static int usb_save_serial(const char *serialno)
{
	int rv;

	if (!serialno)
		return EC_ERROR_INVAL;

	/* Save this new serial number to flash. */
	rv = flash_write_serial(serialno);
	if (rv)
		return rv;

	/* Load this new serial number to memory. */
	rv = usb_load_serial();
	return rv;
}

static int command_serialno(int argc, char **argv)
{
	struct usb_string_desc *sd = usb_serialno_desc;
	char buf[USB_STRING_LEN];
	int rv = EC_SUCCESS;
	int i;

	if (argc != 1) {
		if ((strcasecmp(argv[1], "set") == 0) &&
		    (argc == 3)) {
			ccprintf("Saving serial number\n");
			rv = usb_save_serial(argv[2]);
		} else if ((strcasecmp(argv[1], "load") == 0) &&
			   (argc == 2)) {
			ccprintf("Loading serial number\n");
			rv = usb_load_serial();
		} else
			return EC_ERROR_INVAL;
	}

	for (i = 0; i < USB_STRING_LEN; i++)
		buf[i] = sd->_data[i];
	ccprintf("Serial number: %s\n", buf);
	return rv;
}

DECLARE_CONSOLE_COMMAND(serialno, command_serialno,
	"load/set [value]",
	"Read and write USB serial number");
#endif
