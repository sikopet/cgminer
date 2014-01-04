/*
 * Copyright 2013 Con Kolivas
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

#include "config.h"

#include "miner.h"
#include "driver-bitfury.h"
#include "sha2.h"
#include "mcp2210.h"
#include "libbitfury.h"

int opt_bxf_temp_target = BXF_TEMP_TARGET / 10;

/* Wait longer 1/3 longer than it would take for a full nonce range */
#define BF1WAIT 1600
#define BF1MSGSIZE 7
#define BF1INFOSIZE 14

static void bf1_empty_buffer(struct cgpu_info *bitfury)
{
	char buf[512];
	int amount;

	do {
		usb_read_once(bitfury, buf, 512, &amount, C_BF1_FLUSH);
	} while (amount);
}

static bool bf1_open(struct cgpu_info *bitfury)
{
	uint32_t buf[2];
	int err;

	bf1_empty_buffer(bitfury);
	/* Magic sequence to reset device only really needed for windows but
	 * harmless on linux. */
	buf[0] = 0x80250000;
	buf[1] = 0x00000800;
	err = usb_transfer(bitfury, 0, 9, 1, 0, C_ATMEL_RESET);
	if (!err)
		err = usb_transfer(bitfury, 0x21, 0x22, 0, 0, C_ATMEL_OPEN);
	if (!err) {
		err = usb_transfer_data(bitfury, 0x21, 0x20, 0x0000, 0, buf,
					BF1MSGSIZE, C_ATMEL_INIT);
	}

	if (err < 0) {
		applog(LOG_INFO, "%s %d: Failed to open with error %s", bitfury->drv->name,
		       bitfury->device_id, libusb_error_name(err));
	}
	return (err == BF1MSGSIZE);
}

static void bf1_close(struct cgpu_info *bitfury)
{
	bf1_empty_buffer(bitfury);
}

static void bf1_identify(struct cgpu_info *bitfury)
{
	int amount;

	usb_write(bitfury, "L", 1, &amount, C_BF1_IDENTIFY);
}

static void bitfury_identify(struct cgpu_info *bitfury)
{
	struct bitfury_info *info = bitfury->device_data;

	switch(info->ident) {
		case IDENT_BF1:
			bf1_identify(bitfury);
			break;
		case IDENT_BXF:
		default:
			break;
	}
}

static bool bf1_getinfo(struct cgpu_info *bitfury, struct bitfury_info *info)
{
	int amount, err;
	char buf[16];

	err = usb_write(bitfury, "I", 1, &amount, C_BF1_REQINFO);
	if (err) {
		applog(LOG_INFO, "%s %d: Failed to write REQINFO",
		       bitfury->drv->name, bitfury->device_id);
		return false;
	}
	err = usb_read(bitfury, buf, BF1INFOSIZE, &amount, C_BF1_GETINFO);
	if (err) {
		applog(LOG_INFO, "%s %d: Failed to read GETINFO",
		       bitfury->drv->name, bitfury->device_id);
		return false;
	}
	if (amount != BF1INFOSIZE) {
		applog(LOG_INFO, "%s %d: Getinfo received %d bytes instead of %d",
		       bitfury->drv->name, bitfury->device_id, amount, BF1INFOSIZE);
		return false;
	}
	info->version = buf[1];
	memcpy(&info->product, buf + 2, 8);
	memcpy(&info->serial, buf + 10, 4);

	applog(LOG_INFO, "%s %d: Getinfo returned version %d, product %s serial %08x", bitfury->drv->name,
	       bitfury->device_id, info->version, info->product, info->serial);
	bf1_empty_buffer(bitfury);
	return true;
}

static bool bf1_reset(struct cgpu_info *bitfury)
{
	int amount, err;
	char buf[16];

	err = usb_write(bitfury, "R", 1, &amount, C_BF1_REQRESET);
	if (err) {
		applog(LOG_INFO, "%s %d: Failed to write REQRESET",
		       bitfury->drv->name, bitfury->device_id);
		return false;
	}
	err = usb_read_timeout(bitfury, buf, BF1MSGSIZE, &amount, BF1WAIT,
			       C_BF1_GETRESET);
	if (err) {
		applog(LOG_INFO, "%s %d: Failed to read GETRESET",
		       bitfury->drv->name, bitfury->device_id);
		return false;
	}
	if (amount != BF1MSGSIZE) {
		applog(LOG_INFO, "%s %d: Getreset received %d bytes instead of %d",
		       bitfury->drv->name, bitfury->device_id, amount, BF1MSGSIZE);
		return false;
	}
	applog(LOG_DEBUG, "%s %d: Getreset returned %s", bitfury->drv->name,
	       bitfury->device_id, buf);
	bf1_empty_buffer(bitfury);
	return true;
}

static bool bxf_send_msg(struct cgpu_info *bitfury, char *buf, enum usb_cmds cmd)
{
	int err, amount, len;

	if (unlikely(bitfury->usbinfo.nodev))
		return false;

	len = strlen(buf);
	applog(LOG_DEBUG, "%s %d: Sending %s", bitfury->drv->name, bitfury->device_id, buf);
	err = usb_write(bitfury, buf, len, &amount, cmd);
	if (err || amount != len) {
		applog(LOG_WARNING, "%s %d: Error %d sending %s sent %d of %d", bitfury->drv->name,
		       bitfury->device_id, err, usb_cmdname(cmd), amount, len);
		return false;
	}
	return true;
}

/* Returns the amount received only if we receive a full message, otherwise
 * it returns the err value. */
static int bxf_recv_msg(struct cgpu_info *bitfury, char *buf)
{
	int err, amount;

	err = usb_read_nl(bitfury, buf, 512, &amount, C_BXF_READ);
	if (amount)
		applog(LOG_DEBUG, "%s %d: Received %s", bitfury->drv->name, bitfury->device_id, buf);
	if (!err)
		return amount;
	return err;
}

/* Keep reading till the first timeout or error */
static void bxf_clear_buffer(struct cgpu_info *bitfury)
{
	int err, retries = 0;
	char buf[512];

	do {
		err = bxf_recv_msg(bitfury, buf);
		usb_buffer_clear(bitfury);
		if (err < 0)
			break;
	} while (retries++ < 10);
}

static bool bxf_send_flush(struct cgpu_info *bitfury)
{
	char buf[8];

	sprintf(buf, "flush\n");
	return bxf_send_msg(bitfury, buf, C_BXF_FLUSH);
}

static bool bxf_detect_one(struct cgpu_info *bitfury, struct bitfury_info *info)
{
	int err, retries = 0;
	char buf[512];

	if (!bxf_send_flush(bitfury))
		return false;

	bxf_clear_buffer(bitfury);

	sprintf(buf, "version\n");
	if (!bxf_send_msg(bitfury, buf, C_BXF_VERSION))
		return false;

	do {
		err = bxf_recv_msg(bitfury, buf);
		if (err < 0 && err != LIBUSB_ERROR_TIMEOUT)
			return false;
		if (err > 0 && !strncmp(buf, "version", 7)) {
			sscanf(&buf[8], "%d.%d rev %d chips %d", &info->ver_major,
			       &info->ver_minor, &info->hw_rev, &info->chips);
			applog(LOG_INFO, "%s %d: Version %d.%d rev %d chips %d",
			       bitfury->drv->name, bitfury->device_id, info->ver_major,
			       info->ver_minor, info->hw_rev, info->chips);
			break;
		}
		/* Keep parsing if the buffer is full without counting it as
		 * a retry. */
		if (usb_buffer_size(bitfury))
			continue;
	} while (retries++ < 10);

	if (!add_cgpu(bitfury))
		quit(1, "Failed to add_cgpu in bxf_detect_one");

	update_usb_stats(bitfury);
	applog(LOG_INFO, "%s %d: Successfully initialised %s",
	       bitfury->drv->name, bitfury->device_id, bitfury->device_path);

	info->total_nonces = 1;
	info->temp_target = opt_bxf_temp_target * 10;
	/* This unsets it to make sure it gets set on the first pass */
	info->maxroll = -1;

	return true;
}

static bool bf1_detect_one(struct cgpu_info *bitfury, struct bitfury_info *info)
{
	if (!bf1_open(bitfury))
		goto out_close;

	/* Send getinfo request */
	if (!bf1_getinfo(bitfury, info))
		goto out_close;

	/* Send reset request */
	if (!bf1_reset(bitfury))
		goto out_close;

	bf1_identify(bitfury);
	bf1_empty_buffer(bitfury);

	if (!add_cgpu(bitfury))
		quit(1, "Failed to add_cgpu in bf1_detect_one");

	update_usb_stats(bitfury);
	applog(LOG_INFO, "%s %d: Successfully initialised %s",
	       bitfury->drv->name, bitfury->device_id, bitfury->device_path);

	/* This does not artificially raise hashrate, it simply allows the
	 * hashrate to adapt quickly on starting. */
	info->total_nonces = 1;

	return true;
out_close:
	bf1_close(bitfury);
	return false;
}

static void nf1_close(struct cgpu_info *bitfury)
{
	struct gpio_pin gp;
	int i;

	/* Set all pins to input mode, ignoring return code */
	for (i = 0; i < 9; i++)
		gp.pin[i] = MCP2210_GPIO_INPUT;
	mcp2210_set_gpio_pindirs(bitfury, &gp);
}

static void spi_clear_buf(struct bitfury_info *info)
{
	info->spibufsz = 0;
}

static void spi_add_buf(struct bitfury_info *info, const void *buf, const int sz)
{
	if (unlikely(info->spibufsz + sz > NF1_SPIBUF_SIZE)) {
		applog(LOG_WARNING, "SPI bufsize overflow!");
		return;
	}
	memcpy(&info->spibuf[info->spibufsz], buf, sz);
	info->spibufsz += sz;
}

static void spi_add_break(struct bitfury_info *info)
{
	spi_add_buf(info, "\x4", 1);
}

static void spi_add_buf_reverse(struct bitfury_info *info, const char *buf, const int sz)
{
	int i;

	for (i = 0; i < sz; i++) { // Reverse bit order in each byte!
		unsigned char p = buf[i];

		p = ((p & 0xaa) >> 1) | ((p & 0x55) << 1);
		p = ((p & 0xcc) >> 2) | ((p & 0x33) << 2);
		p = ((p & 0xf0) >> 4) | ((p & 0x0f) << 4);
		info->spibuf[info->spibufsz + i] = p;
	}
	info->spibufsz += sz;
}

static void spi_add_data(struct bitfury_info *info, uint16_t addr, const void *buf, int len)
{
	unsigned char otmp[3];

	if (len < 4 || len > 128) {
		applog(LOG_WARNING, "Can't add SPI data size %d", len);
		return;
	}
	len /= 4; /* Strip */
	otmp[0] = (len - 1) | 0xE0;
	otmp[1] = addr >> 8;
	otmp[2] = addr & 0xFF;
	spi_add_buf(info, otmp, 3);
	len *= 4;
	spi_add_buf_reverse(info, buf, len);
}

/* Configuration registers - control oscillators and such stuff. PROGRAMMED when
 * magic number matches, UNPROGRAMMED (default) otherwise */
static void nf1_config_reg(struct bitfury_info *info, int cfgreg, int ena)
{
	static const uint8_t enaconf[4] = { 0xc1, 0x6a, 0x59, 0xe3 };
	static const uint8_t disconf[4] = { 0, 0, 0, 0 };

	if (ena)
		spi_add_data(info, 0x7000 + cfgreg * 32, enaconf, 4);
	else
		spi_add_data(info, 0x7000 + cfgreg * 32, disconf, 4);
}

static void nf1_set_freq(struct bitfury_info *info)
{
	uint64_t freq;
	const uint8_t *osc6 = (unsigned char *)&freq;

	freq = (1ULL << info->osc6_bits) - 1ULL;
	spi_add_data(info, 0x6000, osc6, 8); /* Program internal on-die slow oscillator frequency */
	nf1_config_reg(info, 4, 1);
}

#define FIRST_BASE 61
#define SECOND_BASE 4

static void nf1_send_conf(struct bitfury_info *info)
{
	const int8_t nf1_counters[16] = { 64, 64, SECOND_BASE, SECOND_BASE+4, SECOND_BASE+2,
		SECOND_BASE+2+16, SECOND_BASE, SECOND_BASE+1, (FIRST_BASE)%65, (FIRST_BASE+1)%65,
		(FIRST_BASE+3)%65, (FIRST_BASE+3+16)%65, (FIRST_BASE+4)%65, (FIRST_BASE+4+4)%65,
		(FIRST_BASE+3+3)%65, (FIRST_BASE+3+1+3)%65 };
	int i;

	for (i = 7; i <= 11; i++)
		nf1_config_reg(info, i, 0);
	nf1_config_reg(info, 6, 0); /* disable OUTSLK */
	nf1_config_reg(info, 4, 1); /* Enable slow oscillator */
	for (i = 1; i <= 3; ++i)
		nf1_config_reg(info, i, 0);
	/* Program counters correctly for rounds processing, here it should
	 * start consuming power */
	spi_add_data(info, 0x0100, nf1_counters, 16);
}

static void nf1_send_init(struct bitfury_info *info)
{
	/* Prepare internal buffers */
	/* PREPARE BUFFERS (INITIAL PROGRAMMING) */
	unsigned int w[16];
	unsigned int atrvec[] = {
		0xb0e72d8e, 0x1dc5b862, 0xe9e7c4a6, 0x3050f1f5, 0x8a1a6b7e, 0x7ec384e8, 0x42c1c3fc, 0x8ed158a1, /* MIDSTATE */
		0,0,0,0,0,0,0,0,
		0x8a0bb7b7, 0x33af304f, 0x0b290c1a, 0xf0c4e61f, /* WDATA: hashMerleRoot[7], nTime, nBits, nNonce */
        };
	ms3steps(atrvec);
	memset(&w, 0, sizeof(w));
	w[3] = 0xffffffff;
	w[4] = 0x80000000;
	w[15] = 0x00000280;
	spi_add_data(info, 0x1000, w, 16 * 4);
	spi_add_data(info, 0x1400, w, 8 * 4);
	memset(w, 0, sizeof(w));
	w[0] = 0x80000000;
	w[7] = 0x100;
	spi_add_data(info, 0x1900, w, 8 * 4); /* Prepare MS and W buffers! */
	spi_add_data(info, 0x3000, atrvec, 19 * 4);
}

// Bit-banging reset... Each 3 reset cycles reset first chip in chain
static bool nf1_spi_reset(struct cgpu_info *bitfury)
{
	char buf[1] = {0x81}; // will send this waveform: - _ _ _ _ _ _ -
	int r;

	// SCK_OVRRIDE
	if (!mcp2210_set_gpio_output(bitfury, NF1_PIN_SCK_OVR, MCP2210_GPIO_PIN_HIGH))
		return false;

	for (r = 0; r < 16; ++r) {
		unsigned int length = 1;
		if (!mcp2210_spi_transfer(bitfury, buf, &length))
			return false;
	}

	if (!mcp2210_set_gpio_input(bitfury, NF1_PIN_SCK_OVR))
		return false;

	return true;
}

static bool nf1_spi_txrx(struct cgpu_info *bitfury, struct bitfury_info *info)
{
	unsigned int length, sendrcv;
	int offset = 0, roffset = 0;

	if (!nf1_spi_reset(bitfury))
		return false;
	length = info->spibufsz;
	applog(LOG_DEBUG, "%s %d: SPI sending %u bytes", bitfury->drv->name, bitfury->device_id,
	       length);
	while (length > MCP2210_TRANSFER_MAX) {
		sendrcv = MCP2210_TRANSFER_MAX;
		if (!mcp2210_spi_transfer(bitfury, info->spibuf + offset, &sendrcv))
			return false;
		if (sendrcv != MCP2210_TRANSFER_MAX) {
			applog(LOG_DEBUG, "%s %d: Send/Receive size mismatch sent %d received %d",
			       bitfury->drv->name, bitfury->device_id, MCP2210_TRANSFER_MAX, sendrcv);
		}
		length -= MCP2210_TRANSFER_MAX;
		offset += MCP2210_TRANSFER_MAX;
		roffset += sendrcv;
	}
	sendrcv = length;
	if (!mcp2210_spi_transfer(bitfury, info->spibuf + offset, &sendrcv))
		return false;
	if (sendrcv != length) {
		applog(LOG_WARNING, "%s %d: Send/Receive size mismatch sent %d received %d",
		       bitfury->drv->name, bitfury->device_id, length, sendrcv);
	}
	roffset += sendrcv;
	info->spibufsz = roffset;
	return true;
}

static void nf1_reinit(struct cgpu_info *bitfury, struct bitfury_info *info)
{
	spi_clear_buf(info);
	spi_add_break(info);
	nf1_set_freq(info);
	nf1_send_conf(info);
	nf1_send_init(info);
	nf1_spi_txrx(bitfury, info);
}

static bool nf1_set_spi_settings(struct cgpu_info *bitfury, struct bitfury_info *info)
{
	struct mcp_settings *mcp = &info->mcp;

	return mcp2210_set_spi_transfer_settings(bitfury, mcp->bitrate, mcp->icsv,
		mcp->acsv, mcp->cstdd, mcp->ldbtcsd, mcp->sdbd, mcp->bpst, mcp->spimode);
}

static bool nf1_detect_one(struct cgpu_info *bitfury, struct bitfury_info *info)
{
	char buf[MCP2210_BUFFER_LENGTH];
	unsigned int length;
	struct gpio_pin gp;
	bool ret = false;
	int i, val;

	/* Set all pins to GPIO mode */
	for (i = 0; i < 9; i++)
		gp.pin[i] = MCP2210_PIN_GPIO;
	if (!mcp2210_set_gpio_pin_designations(bitfury, &gp))
		goto out;

	/* Set all pins to input mode */
	for (i = 0; i < 9; i++)
		gp.pin[i] = MCP2210_GPIO_INPUT;
	if (!mcp2210_set_gpio_pindirs(bitfury, &gp))

	/* Set LED and PWR pins to output and high */
	if (!mcp2210_set_gpio_output(bitfury, NF1_PIN_LED, MCP2210_GPIO_PIN_HIGH))
		goto out;
	if (!mcp2210_set_gpio_output(bitfury, NF1_PIN_PWR_EN, MCP2210_GPIO_PIN_HIGH))
		goto out;

	if (opt_debug) {
		struct gpio_pin gp;

		mcp2210_get_gpio_pindirs(bitfury, &gp);
		for (i = 0; i < 9; i++) {
			applog(LOG_DEBUG, "%s %d: Pin dir %d %d", bitfury->drv->name,
			       bitfury->device_id, i, gp.pin[i]);
		}
		mcp2210_get_gpio_pinvals(bitfury, &gp);
		for (i = 0; i < 9; i++) {
			applog(LOG_DEBUG, "%s %d: Pin val %d %d", bitfury->drv->name,
			       bitfury->device_id, i, gp.pin[i]);
		}
		mcp2210_get_gpio_pindes(bitfury, &gp);
		for (i = 0; i < 9; i++) {
			applog(LOG_DEBUG, "%s %d: Pin des %d %d", bitfury->drv->name,
			       bitfury->device_id, i, gp.pin[i]);
		}
	}

	/* Cancel any transfers in progress */
	if (!mcp2210_spi_cancel(bitfury))
		goto out;
	if (!mcp2210_get_spi_transfer_settings(bitfury, &info->mcp.bitrate, &info->mcp.icsv,
	    &info->mcp.acsv, &info->mcp.cstdd, &info->mcp.ldbtcsd, &info->mcp.sdbd,
	    &info->mcp.bpst, &info->mcp.spimode))
		goto out;
	info->mcp.bitrate = 12000000;
	info->mcp.icsv = 0xffff;
	info->mcp.acsv = 0xffef;
	info->mcp.cstdd = info->mcp.ldbtcsd = info->mcp.sdbd = info->mcp.spimode = 0;
	info->mcp.sdbd = 1;
	if (!nf1_set_spi_settings(bitfury, info))
		goto out;

	buf[0] = 0;
	length = 1;
	if (!mcp2210_spi_transfer(bitfury, buf, &length))
		goto out;
	/* after this command SCK_OVRRIDE should read the same as current SCK
	 * value (which for mode 0 should be 0) */
	if (!mcp2210_get_gpio_pinval(bitfury, NF1_PIN_SCK_OVR, &val))
		goto out;
	if (val != MCP2210_GPIO_PIN_LOW)
		goto out;

	/* switch SCK to polarity (default SCK=1 in mode 2) */
	info->mcp.spimode = 2;
	if (!nf1_set_spi_settings(bitfury, info))
		goto out;
	buf[0] = 0;
	length = 1;
	if (!mcp2210_spi_transfer(bitfury, buf, &length))
		goto out;
	/* after this command SCK_OVRRIDE should read the same as current SCK
	 * value (which for mode 2 should be 1) */
	if (!mcp2210_get_gpio_pinval(bitfury, NF1_PIN_SCK_OVR, &val))
		goto out;
	if (val != MCP2210_GPIO_PIN_HIGH)
		goto out;

	/* switch SCK to polarity (default SCK=0 in mode 0) */
	info->mcp.spimode = 0;
	if (!nf1_set_spi_settings(bitfury, info))
		goto out;
	buf[0] = 0;
	length = 1;
	if (!mcp2210_spi_transfer(bitfury, buf, &length))
		goto out;
	if (!mcp2210_get_gpio_pinval(bitfury, NF1_PIN_SCK_OVR, &val))
		goto out;
	if (val != MCP2210_GPIO_PIN_LOW)
		goto out;

	info->osc6_bits = 50;
	nf1_reinit(bitfury, info);
	ret = true;
out:
	if (!ret)
		nf1_close(bitfury);

	return ret;
}

static struct cgpu_info *bitfury_detect_one(struct libusb_device *dev, struct usb_find_devices *found)
{
	struct cgpu_info *bitfury;
	struct bitfury_info *info;
	enum sub_ident ident;
	bool ret = false;

	bitfury = usb_alloc_cgpu(&bitfury_drv, 1);

	if (!usb_init(bitfury, dev, found))
		goto out;
	applog(LOG_INFO, "%s %d: Found at %s", bitfury->drv->name,
	       bitfury->device_id, bitfury->device_path);

	info = calloc(sizeof(struct bitfury_info), 1);
	if (!info)
		quit(1, "Failed to calloc info in bitfury_detect_one");
	bitfury->device_data = info;
	info->ident = ident = usb_ident(bitfury);
	switch (ident) {
		case IDENT_BF1:
			ret = bf1_detect_one(bitfury, info);
			break;
		case IDENT_BXF:
			ret = bxf_detect_one(bitfury, info);
			break;
		case IDENT_NF1:
			ret = nf1_detect_one(bitfury, info);
			break;
		default:
			applog(LOG_INFO, "%s %d: Unrecognised bitfury device",
			       bitfury->drv->name, bitfury->device_id);
			break;
	}

	if (!ret) {
		free(info);
		usb_uninit(bitfury);
out:
		bitfury = usb_free_cgpu(bitfury);
	}
	return bitfury;
}

static void bitfury_detect(bool __maybe_unused hotplug)
{
	usb_detect(&bitfury_drv, bitfury_detect_one);
}

static void parse_bxf_submit(struct cgpu_info *bitfury, struct bitfury_info *info, char *buf)
{
	struct work *match_work, *tmp, *work = NULL;
	struct thr_info *thr = info->thr;
	uint32_t nonce, timestamp;
	int workid, chip = -1;

	if (!sscanf(&buf[7], "%x %x %x %d", &nonce, &workid, &timestamp, &chip)) {
		applog(LOG_WARNING, "%s %d: Failed to parse submit response",
		       bitfury->drv->name, bitfury->device_id);
		return;
	}
	if (chip > -1 && chip < 2)
		info->submits[chip]++;

	applog(LOG_DEBUG, "%s %d: Parsed nonce %u workid %d timestamp %u",
	       bitfury->drv->name, bitfury->device_id, nonce, workid, timestamp);

	rd_lock(&bitfury->qlock);
	HASH_ITER(hh, bitfury->queued_work, match_work, tmp) {
		if (match_work->subid == workid) {
			work = copy_work(match_work);
			break;
		}
	}
	rd_unlock(&bitfury->qlock);

	if (!work) {
		/* Discard first results from any previous run */
		if (unlikely(!info->valid))
			return;

		applog(LOG_INFO, "%s %d: No matching work", bitfury->drv->name, bitfury->device_id);

		mutex_lock(&info->lock);
		info->no_matching_work++;
		mutex_unlock(&info->lock);

		inc_hw_errors(thr);
		return;
	}
	/* Set the device start time from when we first get valid results */
	if (unlikely(!info->valid)) {
		info->valid = true;
		cgtime(&bitfury->dev_start_tv);
	}
	set_work_ntime(work, timestamp);
	if (submit_nonce(thr, work, nonce)) {
		mutex_lock(&info->lock);
		info->nonces++;
		mutex_unlock(&info->lock);
	}
	free_work(work);
}

static bool bxf_send_clock(struct cgpu_info *bitfury, struct bitfury_info *info,
			   uint8_t clockspeed)
{
	char buf[64];

	info->clocks = clockspeed;
	sprintf(buf, "clock %d %d\n", clockspeed, clockspeed);
	return bxf_send_msg(bitfury, buf, C_BXF_CLOCK);
}

static void parse_bxf_temp(struct cgpu_info *bitfury, struct bitfury_info *info, char *buf)
{
	uint8_t clockspeed = info->clocks;
	int decitemp;

	if (!sscanf(&buf[5], "%d", &decitemp)) {
		applog(LOG_INFO, "%s %d: Failed to parse temperature",
		       bitfury->drv->name, bitfury->device_id);
		return;
	}

	mutex_lock(&info->lock);
	info->temperature = (double)decitemp / 10;
	if (decitemp > info->max_decitemp) {
		info->max_decitemp = decitemp;
		applog(LOG_DEBUG, "%s %d: New max decitemp %d", bitfury->drv->name,
		       bitfury->device_id, decitemp);
	}
	mutex_unlock(&info->lock);

	if (decitemp > info->temp_target + BXF_TEMP_HYSTERESIS) {
		if (info->clocks <= BXF_CLOCK_MIN)
			goto out;
		applog(LOG_WARNING, "%s %d: Hit overheat temperature of %d, throttling!",
		       bitfury->drv->name, bitfury->device_id, decitemp);
		clockspeed = BXF_CLOCK_MIN;
		goto out;
	}
	if (decitemp > info->temp_target) {
		if (info->clocks <= BXF_CLOCK_MIN)
			goto out;
		if (decitemp < info->last_decitemp)
			goto out;
		applog(LOG_INFO, "%s %d: Temp %d over target and not falling, decreasing clock",
		       bitfury->drv->name, bitfury->device_id, decitemp);
		clockspeed = info->clocks - 1;
		goto out;
	}
	if (decitemp <= info->temp_target && decitemp >= info->temp_target - BXF_TEMP_HYSTERESIS) {
		if (decitemp == info->last_decitemp)
			goto out;
		if (decitemp > info->last_decitemp) {
			if (info->clocks <= BXF_CLOCK_MIN)
				goto out;
			applog(LOG_DEBUG, "%s %d: Temp %d in target and rising, decreasing clock",
			       bitfury->drv->name, bitfury->device_id, decitemp);
			clockspeed = info->clocks - 1;
			goto out;
		}
		/* implies: decitemp < info->last_decitemp */
		if (info->clocks >= BXF_CLOCK_DEFAULT)
			goto out;
		applog(LOG_DEBUG, "%s %d: Temp %d in target and falling, increasing clock",
		       bitfury->drv->name, bitfury->device_id, decitemp);
		clockspeed = info->clocks + 1;
		goto out;
	}
	/* implies: decitemp < info->temp_target - BXF_TEMP_HYSTERESIS */
	if (info->clocks >= BXF_CLOCK_DEFAULT)
		goto out;
	applog(LOG_DEBUG, "%s %d: Temp %d below target, increasing clock",
		bitfury->drv->name, bitfury->device_id, decitemp);
	clockspeed = info->clocks + 1;
out:
	bxf_send_clock(bitfury, info, clockspeed);
	info->last_decitemp = decitemp;
}

static void bxf_update_work(struct cgpu_info *bitfury, struct bitfury_info *info);

static void parse_bxf_needwork(struct cgpu_info *bitfury, struct bitfury_info *info,
			       char *buf)
{
	int needed;

	if (!sscanf(&buf[9], "%d", &needed)) {
		applog(LOG_INFO, "%s %d: Failed to parse needwork",
		       bitfury->drv->name, bitfury->device_id);
		return;
	}
	while (needed-- > 0)
		bxf_update_work(bitfury, info);
}

static void parse_bxf_job(struct cgpu_info *bitfury, struct bitfury_info *info, char *buf)
{
	int job_id, timestamp, chip;

	if (sscanf(&buf[4], "%x %x %x", &job_id, &timestamp, &chip) != 3) {
		applog(LOG_INFO, "%s %d: Failed to parse job",
		       bitfury->drv->name, bitfury->device_id);
		return;
	}
	if (chip > 1) {
		applog(LOG_INFO, "%s %d: Invalid job chip number %d",
		       bitfury->drv->name, bitfury->device_id, chip);
		return;
	}
	++info->job[chip];
}

static void parse_bxf_hwerror(struct cgpu_info *bitfury, struct bitfury_info *info, char *buf)
{
	int chip;

	if (!sscanf(&buf[8], "%d", &chip)) {
		applog(LOG_INFO, "%s %d: Failed to parse hwerror",
		       bitfury->drv->name, bitfury->device_id);
		return;
	}
	if (chip > 1) {
		applog(LOG_INFO, "%s %d: Invalid hwerror chip number %d",
		       bitfury->drv->name, bitfury->device_id, chip);
		return;
	}
	++info->filtered_hw[chip];
}

#define PARSE_BXF_MSG(MSG) \
	msg = strstr(buf, #MSG); \
	if (msg) { \
		parse_bxf_##MSG(bitfury, info, msg); \
		continue; \
	}

static void *bxf_get_results(void *userdata)
{
	struct cgpu_info *bitfury = userdata;
	struct bitfury_info *info = bitfury->device_data;
	char threadname[24], buf[512];

	snprintf(threadname, 24, "bxf_recv/%d", bitfury->device_id);

	/* We operate the device at lowest diff since it's not a lot of results
	 * to process and gives us a better indicator of the nonce return rate
	 * and hardware errors. */
	sprintf(buf, "target ffffffff\n");
	if (!bxf_send_msg(bitfury, buf, C_BXF_TARGET))
		goto out;

	/* Read thread sends the first work item to get the device started
	 * since it will roll ntime and make work itself from there on. */
	bxf_update_work(bitfury, info);
	bxf_update_work(bitfury, info);

	while (likely(!bitfury->shutdown)) {
		char *msg;
		int err;

		if (unlikely(bitfury->usbinfo.nodev))
			break;

		err = bxf_recv_msg(bitfury, buf);
		if (err < 0) {
			if (err != LIBUSB_ERROR_TIMEOUT)
				break;
			continue;
		}
		if (!err)
			continue;

		PARSE_BXF_MSG(submit);
		PARSE_BXF_MSG(temp);
		PARSE_BXF_MSG(needwork);
		PARSE_BXF_MSG(job);
		PARSE_BXF_MSG(hwerror);

		applog(LOG_DEBUG, "%s %d: Unrecognised string %s",
		       bitfury->drv->name, bitfury->device_id, buf);
	}
out:
	return NULL;
}

static bool bxf_prepare(struct cgpu_info *bitfury, struct bitfury_info *info)
{
	mutex_init(&info->lock);
	if (pthread_create(&info->read_thr, NULL, bxf_get_results, (void *)bitfury))
		quit(1, "Failed to create bxf read_thr");
	return bxf_send_clock(bitfury, info, BXF_CLOCK_DEFAULT);
}

static bool bitfury_prepare(struct thr_info *thr)
{
	struct cgpu_info *bitfury = thr->cgpu;
	struct bitfury_info *info = bitfury->device_data;

	info->thr = thr;

	switch(info->ident) {
		case IDENT_BXF:
			return bxf_prepare(bitfury, info);
			break;
		case IDENT_BF1:
		default:
			return true;
	}
}

#define BT_OFFSETS 3
const uint32_t bf_offsets[] = {-0x800000, 0, -0x400000};

static bool bitfury_checkresults(struct thr_info *thr, struct work *work, uint32_t nonce)
{
	int i;

	for (i = 0; i < BT_OFFSETS; i++) {
		uint32_t noffset = nonce + bf_offsets[i];

		if (test_nonce(work, noffset)) {
			submit_tested_work(thr, work);
			return true;
		}
	}
	return false;
}

static int64_t bitfury_rate(struct bitfury_info *info)
{
	double nonce_rate;
	int64_t ret = 0;

	info->cycles++;
	info->total_nonces += info->nonces;
	info->saved_nonces += info->nonces;
	info->nonces = 0;
	nonce_rate = (double)info->total_nonces / (double)info->cycles;
	if (info->saved_nonces >= nonce_rate) {
		info->saved_nonces -= nonce_rate;
		ret = (double)0xffffffff * nonce_rate;
	}
	return ret;
}

static int64_t bf1_scan(struct thr_info *thr, struct cgpu_info *bitfury,
			struct bitfury_info *info)
{
	int amount, i, aged, total = 0, ms_diff;
	char readbuf[512], buf[45];
	struct work *work, *tmp;
	struct timeval tv_now;
	int64_t ret = 0;

	work = get_queue_work(thr, bitfury, thr->id);
	if (unlikely(thr->work_restart)) {
		work_completed(bitfury, work);
		goto out;
	}

	buf[0] = 'W';
	memcpy(buf + 1, work->midstate, 32);
	memcpy(buf + 33, work->data + 64, 12);

	/* New results may spill out from the latest work, making us drop out
	 * too early so read whatever we get for the first half nonce and then
	 * look for the results to prev work. */
	cgtime(&tv_now);
	ms_diff = 600 - ms_tdiff(&tv_now, &info->tv_start);
	if (ms_diff > 0) {
		usb_read_timeout_cancellable(bitfury, readbuf, 512, &amount, ms_diff,
					     C_BF1_GETRES);
		total += amount;
	}

	/* Now look for the bulk of the previous work results, they will come
	 * in a batch following the first data. */
	cgtime(&tv_now);
	ms_diff = BF1WAIT - ms_tdiff(&tv_now, &info->tv_start);
	/* If a work restart was sent, just empty the buffer. */
	if (unlikely(ms_diff < 10 || thr->work_restart))
		ms_diff = 10;
	usb_read_once_timeout_cancellable(bitfury, readbuf + total, BF1MSGSIZE,
					  &amount, ms_diff, C_BF1_GETRES);
	total += amount;
	while (amount) {
		usb_read_once_timeout(bitfury, readbuf + total, 512 - total, &amount, 10,
				      C_BF1_GETRES);
		total += amount;
	};

	/* Don't send whatever work we've stored if we got a restart */
	if (unlikely(thr->work_restart))
		goto out;

	/* Send work */
	cgtime(&work->tv_work_start);
	usb_write(bitfury, buf, 45, &amount, C_BF1_REQWORK);
	cgtime(&info->tv_start);

	/* Get response acknowledging work */
	usb_read(bitfury, buf, BF1MSGSIZE, &amount, C_BF1_GETWORK);

out:
	/* Search for what work the nonce matches in order of likelihood. Last
	 * entry is end of result marker. */
	for (i = 0; i < total - BF1MSGSIZE; i += BF1MSGSIZE) {
		bool found = false;
		uint32_t nonce;

		/* Ignore state & switched data in results for now. */
		memcpy(&nonce, readbuf + i + 3, 4);
		nonce = decnonce(nonce);

		rd_lock(&bitfury->qlock);
		HASH_ITER(hh, bitfury->queued_work, work, tmp) {
			if (bitfury_checkresults(thr, work, nonce)) {
				info->nonces++;
				found = true;
				break;
			}
		}
		rd_unlock(&bitfury->qlock);

		if (!found) {
			if (likely(info->valid))
				inc_hw_errors(thr);
		} else if (unlikely(!info->valid)) {
			info->valid = true;
			cgtime(&bitfury->dev_start_tv);
		}
	}

	cgtime(&tv_now);

	/* This iterates over the hashlist finding work started more than 6
	 * seconds ago. */
	aged = age_queued_work(bitfury, 6.0);
	if (aged) {
		applog(LOG_DEBUG, "%s %d: Aged %d work items", bitfury->drv->name,
		       bitfury->device_id, aged);
	}

	ret = bitfury_rate(info);

	if (unlikely(bitfury->usbinfo.nodev)) {
		applog(LOG_WARNING, "%s %d: Device disappeared, disabling thread",
		       bitfury->drv->name, bitfury->device_id);
		ret = -1;
	}
	return ret;
}

static int64_t bxf_scan(struct cgpu_info *bitfury, struct bitfury_info *info)
{
	int64_t ret;
	int aged;

	bxf_update_work(bitfury, info);
	cgsleep_ms(600);

	mutex_lock(&info->lock);
	ret = bitfury_rate(info);
	mutex_unlock(&info->lock);

	/* Keep no more than the last 90 seconds worth of work items in the
	 * hashlist */
	aged = age_queued_work(bitfury, 90.0);
	if (aged) {
		applog(LOG_DEBUG, "%s %d: Aged %d work items", bitfury->drv->name,
		       bitfury->device_id, aged);
	}

	if (unlikely(bitfury->usbinfo.nodev)) {
		applog(LOG_WARNING, "%s %d: Device disappeared, disabling thread",
		       bitfury->drv->name, bitfury->device_id);
		ret = -1;
	}
	return ret;
}

static int64_t bitfury_scanwork(struct thr_info *thr)
{
	struct cgpu_info *bitfury = thr->cgpu;
	struct bitfury_info *info = bitfury->device_data;

	switch(info->ident) {
		case IDENT_BF1:
			return bf1_scan(thr, bitfury, info);
			break;
		case IDENT_BXF:
			return bxf_scan(bitfury, info);
			break;
		default:
			return 0;
	}
}

static void bxf_send_maxroll(struct cgpu_info *bitfury, int maxroll)
{
	char buf[20];

	sprintf(buf, "maxroll %d\n", maxroll);
	bxf_send_msg(bitfury, buf, C_BXF_MAXROLL);
}

static bool bxf_send_work(struct cgpu_info *bitfury, struct work *work)
{
	char buf[512], hexwork[156];

	__bin2hex(hexwork, work->data, 76);
	sprintf(buf, "work %s %x\n", hexwork, work->subid);
	return bxf_send_msg(bitfury, buf, C_BXF_WORK);
}

static void bxf_update_work(struct cgpu_info *bitfury, struct bitfury_info *info)
{
	struct thr_info *thr = info->thr;
	struct work *work;

	if (unlikely(bitfury->usbinfo.nodev))
		return;

	work = get_queue_work(thr, bitfury, thr->id);
	if (work->drv_rolllimit != info->maxroll) {
		info->maxroll = work->drv_rolllimit;
		bxf_send_maxroll(bitfury, info->maxroll);
	}

	mutex_lock(&info->lock);
	work->subid = ++info->work_id;
	mutex_unlock(&info->lock);

	cgtime(&work->tv_work_start);
	bxf_send_work(bitfury, work);
}

static void bitfury_flush_work(struct cgpu_info *bitfury)
{
	struct bitfury_info *info = bitfury->device_data;

	switch(info->ident) {
		case IDENT_BXF:
			bxf_send_flush(bitfury);
			bxf_update_work(bitfury, info);
			bxf_update_work(bitfury, info);
		case IDENT_BF1:
		default:
			break;
	}
}

static void bitfury_update_work(struct cgpu_info *bitfury)
{
	struct bitfury_info *info = bitfury->device_data;

	switch(info->ident) {
		case IDENT_BXF:
			bxf_update_work(bitfury, info);
		case IDENT_BF1:
		default:
			break;
	}
}

static struct api_data *bf1_api_stats(struct bitfury_info *info)
{
	struct api_data *root = NULL;
	double nonce_rate;
	char serial[16];
	int version;

	version = info->version;
	root = api_add_int(root, "Version", &version, true);
	root = api_add_string(root, "Product", info->product, false);
	sprintf(serial, "%08x", info->serial);
	root = api_add_string(root, "Serial", serial, true);
	nonce_rate = (double)info->total_nonces / (double)info->cycles;
	root = api_add_double(root, "NonceRate", &nonce_rate, true);

	return root;
}

static struct api_data *bxf_api_stats(struct bitfury_info *info)
{
	struct api_data *root = NULL;
	double nonce_rate;
	char buf[32];

	sprintf(buf, "%d.%d", info->ver_major, info->ver_minor);
	root = api_add_string(root, "Version", buf, true);
	root = api_add_int(root, "Revision", &info->hw_rev,  false);
	root = api_add_int(root, "Chips", &info->chips, false);
	nonce_rate = (double)info->total_nonces / (double)info->cycles;
	root = api_add_double(root, "NonceRate", &nonce_rate, true);
	root = api_add_int(root, "NoMatchingWork", &info->no_matching_work, false);
	root = api_add_double(root, "Temperature", &info->temperature, false);
	root = api_add_int(root, "Max DeciTemp", &info->max_decitemp, false);
	root = api_add_uint8(root, "Clock", &info->clocks, false);
	root = api_add_int(root, "Core0 hwerror", &info->filtered_hw[0], false);
	root = api_add_int(root, "Core1 hwerror", &info->filtered_hw[1], false);
	root = api_add_int(root, "Core0 jobs", &info->job[0], false);
	root = api_add_int(root, "Core1 jobs", &info->job[1], false);
	root = api_add_int(root, "Core0 submits", &info->submits[0], false);
	root = api_add_int(root, "Core1 submits", &info->submits[1], false);

	return root;
}

static struct api_data *bitfury_api_stats(struct cgpu_info *cgpu)
{
	struct bitfury_info *info = cgpu->device_data;

	switch(info->ident) {
		case IDENT_BF1:
			return bf1_api_stats(info);
			break;
		case IDENT_BXF:
			return bxf_api_stats(info);
			break;
		default:
			break;
	}
	return NULL;
}

static void bitfury_get_statline_before(char *buf, size_t bufsiz, struct cgpu_info *cgpu)
{
	struct bitfury_info *info = cgpu->device_data;

	switch(info->ident) {
		case IDENT_BXF:
			tailsprintf(buf, bufsiz, "%5.1fC         | ", info->temperature);
			break;
		case IDENT_BF1:
		default:
			tailsprintf(buf, bufsiz, "               | ");
			break;
	}
}

static void bf1_init(struct cgpu_info *bitfury)
{
	bf1_close(bitfury);
	bf1_open(bitfury);
	bf1_reset(bitfury);
}

static void bitfury_init(struct cgpu_info *bitfury)
{
	struct bitfury_info *info = bitfury->device_data;

	switch(info->ident) {
		case IDENT_BF1:
			bf1_init(bitfury);
			break;
		case IDENT_BXF:
		default:
			break;
	}
}

static void bxf_close(struct bitfury_info *info)
{
	pthread_join(info->read_thr, NULL);
	mutex_destroy(&info->lock);
}

static void bitfury_shutdown(struct thr_info *thr)
{
	struct cgpu_info *bitfury = thr->cgpu;
	struct bitfury_info *info = bitfury->device_data;

	switch(info->ident) {
		case IDENT_BF1:
			bf1_close(bitfury);
			break;
		case IDENT_BXF:
			bxf_close(info);
			break;
		default:
			break;
	}
}

/* Currently hardcoded to BF1 devices */
struct device_drv bitfury_drv = {
	.drv_id = DRIVER_bitfury,
	.dname = "bitfury",
	.name = "BF1",
	.drv_detect = bitfury_detect,
	.thread_prepare = bitfury_prepare,
	.hash_work = &hash_driver_work,
	.scanwork = bitfury_scanwork,
	.flush_work = bitfury_flush_work,
	.update_work = bitfury_update_work,
	.get_api_stats = bitfury_api_stats,
	.get_statline_before = bitfury_get_statline_before,
	.reinit_device = bitfury_init,
	.thread_shutdown = bitfury_shutdown,
	.identify_device = bitfury_identify
};
