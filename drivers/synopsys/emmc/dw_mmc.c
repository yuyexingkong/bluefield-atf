/*
 * Copyright (c) 2016-2017, ARM Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <arch.h>
#include <arch_helpers.h>
#include <assert.h>
#include <debug.h>
#include <delay_timer.h>
#include <dw_mmc.h>
#include <emmc.h>
#include <errno.h>
#include <mmio.h>
#include <string.h>

/*
 * Define DWMMC_NO_DMA in your platform's Makefile to use direct
 * access to the FIFO register rather than DMA.  DMA offloads some of
 * the work from the processor, but requires more code space to
 * implement.  Without DMA, we currently only allow reads of up to the
 * controller's FIFO size, and don't allow writes.
 */

#define DWMMC_CTRL			(0x00)
#define CTRL_IDMAC_EN			(1 << 25)
#define CTRL_DMA_EN			(1 << 5)
#define CTRL_INT_EN			(1 << 4)
#define CTRL_DMA_RESET			(1 << 2)
#define CTRL_FIFO_RESET			(1 << 1)
#define CTRL_RESET			(1 << 0)
#define CTRL_RESET_ALL			(CTRL_DMA_RESET | CTRL_FIFO_RESET | \
					 CTRL_RESET)

#define DWMMC_PWREN			(0x04)
#define DWMMC_CLKDIV			(0x08)
#define DWMMC_CLKSRC			(0x0c)
#define DWMMC_CLKENA			(0x10)
#define DWMMC_TMOUT			(0x14)
#define DWMMC_CTYPE			(0x18)
#define CTYPE_8BIT			(1 << 16)
#define CTYPE_4BIT			(1)
#define CTYPE_1BIT			(0)

#define DWMMC_BLKSIZ			(0x1c)
#define DWMMC_BYTCNT			(0x20)
#define DWMMC_INTMASK			(0x24)
#define INT_EBE				(1 << 15)
#define INT_SBE				(1 << 13)
#define INT_HLE				(1 << 12)
#define INT_FRUN			(1 << 11)
#define INT_DRT				(1 << 9)
#define INT_RTO				(1 << 8)
#define INT_DCRC			(1 << 7)
#define INT_RCRC			(1 << 6)
#define INT_RXDR			(1 << 5)
#define INT_TXDR			(1 << 4)
#define INT_DTO				(1 << 3)
#define INT_CMD_DONE			(1 << 2)
#define INT_RE				(1 << 1)

#define DWMMC_CMDARG			(0x28)
#define DWMMC_CMD			(0x2c)
#define CMD_START			(1 << 31)
#define CMD_USE_HOLD_REG		(1 << 29)	/* 0 if SDR50/100 */
#define CMD_UPDATE_CLK_ONLY		(1 << 21)
#define CMD_SEND_INIT			(1 << 15)
#define CMD_STOP_ABORT_CMD		(1 << 14)
#define CMD_WAIT_PRVDATA_COMPLETE	(1 << 13)
#define CMD_WRITE			(1 << 10)
#define CMD_DATA_TRANS_EXPECT		(1 << 9)
#define CMD_CHECK_RESP_CRC		(1 << 8)
#define CMD_RESP_LEN			(1 << 7)
#define CMD_RESP_EXPECT			(1 << 6)
#define CMD(x)				(x & 0x3f)

#define DWMMC_RESP0			(0x30)
#define DWMMC_RESP1			(0x34)
#define DWMMC_RESP2			(0x38)
#define DWMMC_RESP3			(0x3c)
#define DWMMC_RINTSTS			(0x44)
#define DWMMC_STATUS			(0x48)
#define STATUS_DATA_BUSY		(1 << 9)

#define DWMMC_FIFOTH			(0x4c)
#define FIFOTH_TWMARK(x)		(x & 0xfff)
#define FIFOTH_RWMARK(x)		((x & 0xfff) << 16)
#define FIFOTH_GET_RWMARK(data)		(((data) >> 16) & 0xfff)
#define FIFOTH_DMA_BURST_SIZE(x)	((x & 0x7) << 28)

#define DWMMC_DEBNCE			(0x64)
#define DWMMC_BMOD			(0x80)
#define BMOD_ENABLE			(1 << 7)
#define BMOD_FB				(1 << 1)
#define BMOD_SWRESET			(1 << 0)

#define DWMMC_DBADDR			(0x88)
#define DWMMC_IDSTS			(0x8c)
#define DWMMC_IDINTEN			(0x90)
#define DWMMC_CARDTHRCTL		(0x100)
#define CARDTHRCTL_RD_THR(x)		((x & 0xfff) << 16)
#define CARDTHRCTL_RD_THR_EN		(1 << 0)

#define DWMMC_FIFO			(0x200)

#define IDMAC_DES0_DIC			(1 << 1)
#define IDMAC_DES0_LD			(1 << 2)
#define IDMAC_DES0_FS			(1 << 3)
#define IDMAC_DES0_CH			(1 << 4)
#define IDMAC_DES0_ER			(1 << 5)
#define IDMAC_DES0_CES			(1 << 30)
#define IDMAC_DES0_OWN			(1 << 31)
#define IDMAC_DES1_BS1(x)		((x) & 0x1fff)
#define IDMAC_DES2_BS2(x)		(((x) & 0x1fff) << 13)

#define DWMMC_DMA_MAX_BUFFER_SIZE	(512 * 8)

#define DWMMC_8BIT_MODE			(1 << 6)

#define TIMEOUT				100000

struct dw_idmac_desc {
	unsigned int	des0;
	unsigned int	des1;
	unsigned int	des2;
	unsigned int	des3;
};

static void dw_init(void);
static int dw_send_cmd(emmc_cmd_t *cmd);
static int dw_set_ios(int clk, int width);
static int dw_prepare(int lba, uintptr_t buf, size_t size);
static int dw_read(int lba, uintptr_t buf, size_t size);
static int dw_write(int lba, uintptr_t buf, size_t size);

static const emmc_ops_t dw_mmc_ops = {
	.init		= dw_init,
	.send_cmd	= dw_send_cmd,
	.set_ios	= dw_set_ios,
	.prepare	= dw_prepare,
	.read		= dw_read,
	.write		= dw_write,
};

static dw_mmc_params_t dw_params;
#ifdef DWMMC_NO_DMA
static int dw_fifo_depth;
#endif

static void dw_update_clk(void)
{
	unsigned int data;

	while (1) {
		mmio_write_32(dw_params.reg_base + DWMMC_CMD,
			      CMD_WAIT_PRVDATA_COMPLETE | CMD_UPDATE_CLK_ONLY |
			      CMD_START);
		do {
			/* When CMD_START is cleared, return */
			data = mmio_read_32(dw_params.reg_base + DWMMC_CMD);
			if ((data & CMD_START) == 0)
				return;

			/* If HLE is not set, keep waiting */
			data = mmio_read_32(dw_params.reg_base + DWMMC_RINTSTS);

		} while (!(data & INT_HLE));

		/* Clear HLE and repeat the command */
		mmio_write_32(dw_params.reg_base + DWMMC_RINTSTS, INT_HLE);
	}
}

static void dw_set_clk(int clk)
{
	unsigned int data;
	int div;

	assert(clk > 0);

	for (div = 1; div < 256; div++) {
		if ((dw_params.clk_rate / (2 * div)) <= clk) {
			break;
		}
	}
	assert(div < 256);

	/* wait until controller is idle */
	do {
		data = mmio_read_32(dw_params.reg_base + DWMMC_STATUS);
	} while (data & STATUS_DATA_BUSY);

	/* disable clock before change clock rate */
	mmio_write_32(dw_params.reg_base + DWMMC_CLKENA, 0);
	dw_update_clk();

	mmio_write_32(dw_params.reg_base + DWMMC_CLKDIV, div);
	dw_update_clk();

	/* enable clock */
	mmio_write_32(dw_params.reg_base + DWMMC_CLKENA, 1);
	mmio_write_32(dw_params.reg_base + DWMMC_CLKSRC, 0);
	dw_update_clk();
}

static void dw_init(void)
{
	unsigned int data;
	uintptr_t base;

	assert((dw_params.reg_base & EMMC_BLOCK_MASK) == 0);

	base = dw_params.reg_base;
	mmio_write_32(base + DWMMC_PWREN, 1);
	mmio_write_32(base + DWMMC_CTRL, CTRL_RESET_ALL);
	do {
		data = mmio_read_32(base + DWMMC_CTRL);
	} while (data);

	mmio_write_32(base + DWMMC_RINTSTS, ~0);
	mmio_write_32(base + DWMMC_INTMASK, 0);
	mmio_write_32(base + DWMMC_TMOUT, ~0);
	mmio_write_32(base + DWMMC_BLKSIZ, EMMC_BLOCK_SIZE);
	mmio_write_32(base + DWMMC_BYTCNT, 256 * 1024);
	mmio_write_32(base + DWMMC_DEBNCE, 0x00ffffff);

#ifdef DWMMC_NO_DMA
	/* just enable interrupts */
	mmio_write_32(base + DWMMC_CTRL, CTRL_INT_EN);
#else
	/* enable DMA in CTRL */
	data = CTRL_INT_EN | CTRL_DMA_EN | CTRL_IDMAC_EN;
	mmio_write_32(base + DWMMC_CTRL, data);
	mmio_write_32(base + DWMMC_IDINTEN, ~0);
#endif
	dsbsy();

	mmio_write_32(base + DWMMC_BMOD, BMOD_SWRESET);
	do {
		data = mmio_read_32(base + DWMMC_BMOD);
	} while (data & BMOD_SWRESET);

#ifdef DWMMC_NO_DMA
	/* read the FIFO size for the maximum transaction size */
	data = mmio_read_32(base + DWMMC_FIFOTH);
	dw_fifo_depth = (FIFOTH_GET_RWMARK(data) + 1) * sizeof(int32_t);
#else
	/* enable DMA in BMOD */
	data |= BMOD_ENABLE | BMOD_FB;
	mmio_write_32(base + DWMMC_BMOD, data);
#endif

	udelay(100);
	dw_set_ios(EMMC_BOOT_CLK_RATE, EMMC_BUS_WIDTH_1);
	udelay(100);
}

static int dw_send_cmd(emmc_cmd_t *cmd)
{
	unsigned int op, data, err_mask, pending;
	uintptr_t base;
	int timeout;

	assert(cmd);

	base = dw_params.reg_base;

	switch (cmd->cmd_idx) {
	case EMMC_CMD0:
		op = CMD_SEND_INIT;
		break;
	case EMMC_CMD12:
		op = CMD_STOP_ABORT_CMD;
		break;
	case EMMC_CMD13:
		op = CMD_WAIT_PRVDATA_COMPLETE;
		break;
	case EMMC_CMD8:
	case EMMC_CMD17:
	case EMMC_CMD18:
		op = CMD_DATA_TRANS_EXPECT | CMD_WAIT_PRVDATA_COMPLETE;
		break;
	case EMMC_CMD24:
	case EMMC_CMD25:
#ifdef DWMMC_NO_DMA
		/*
		 * Without DMA, we won't have any data arriving in the
		 * FIFO, and the command will never terminate.  We
		 * could change the API to do write() before prepare()
		 * and then allow pre-filling the FIFO, but for now we
		 * avoid changing the API so we don't break
		 * out-of-tree backends.
		 */
		return -EINVAL;
#else
		op = CMD_WRITE | CMD_DATA_TRANS_EXPECT |
		     CMD_WAIT_PRVDATA_COMPLETE;
		break;
#endif
	default:
		op = 0;
		break;
	}
	op |= CMD_USE_HOLD_REG | CMD_START;
	switch (cmd->resp_type) {
	case 0:
		break;
	case EMMC_RESPONSE_R2:
		op |= CMD_RESP_EXPECT | CMD_CHECK_RESP_CRC |
		      CMD_RESP_LEN;
		break;
	case EMMC_RESPONSE_R3:
		op |= CMD_RESP_EXPECT;
		break;
	default:
		op |= CMD_RESP_EXPECT | CMD_CHECK_RESP_CRC;
		break;
	}
	timeout = TIMEOUT;
	do {
		data = mmio_read_32(base + DWMMC_STATUS);
		if (--timeout <= 0)
			panic();
	} while (data & STATUS_DATA_BUSY);

	mmio_write_32(base + DWMMC_RINTSTS, ~0);
	mmio_write_32(base + DWMMC_CMDARG, cmd->cmd_arg);
	dsbsy();
	mmio_write_32(base + DWMMC_CMD, op | cmd->cmd_idx);

	err_mask = INT_EBE | INT_HLE | INT_RTO | INT_RCRC | INT_RE |
		   INT_DCRC | INT_DRT | INT_SBE;
	timeout = TIMEOUT;
	pending = INT_CMD_DONE | ((op & CMD_DATA_TRANS_EXPECT) ? INT_DTO : 0);
	do {
		udelay(500);
		data = mmio_read_32(base + DWMMC_RINTSTS);

		if (data & err_mask) {
			ERROR("%s, RINTSTS:0x%x\n", __func__, data);
			return -EIO;
		}
		if (data & INT_DTO)
			pending &= ~INT_DTO;
		if (data & INT_CMD_DONE)
			pending &= ~INT_CMD_DONE;
		if (--timeout == 0) {
			ERROR("%s, RINTSTS:0x%x\n", __func__, data);
			panic();
		}
	} while (pending);

	if (op & CMD_RESP_EXPECT) {
		cmd->resp_data[0] = mmio_read_32(base + DWMMC_RESP0);
		if (op & CMD_RESP_LEN) {
			cmd->resp_data[1] = mmio_read_32(base + DWMMC_RESP1);
			cmd->resp_data[2] = mmio_read_32(base + DWMMC_RESP2);
			cmd->resp_data[3] = mmio_read_32(base + DWMMC_RESP3);
		}
	}
	return 0;
}

static int dw_set_ios(int clk, int width)
{
	switch (width) {
	case EMMC_BUS_WIDTH_1:
		mmio_write_32(dw_params.reg_base + DWMMC_CTYPE, CTYPE_1BIT);
		break;
	case EMMC_BUS_WIDTH_4:
		mmio_write_32(dw_params.reg_base + DWMMC_CTYPE, CTYPE_4BIT);
		break;
	case EMMC_BUS_WIDTH_8:
		mmio_write_32(dw_params.reg_base + DWMMC_CTYPE, CTYPE_8BIT);
		break;
	default:
		assert(0);
		break;
	}
	dw_set_clk(clk);
	return 0;
}

static void init_dma(uintptr_t buf, size_t size)
{
#ifndef DWMMC_NO_DMA
	struct dw_idmac_desc *desc;
	int desc_cnt, i, last;
	uintptr_t base;

	desc_cnt = (size + DWMMC_DMA_MAX_BUFFER_SIZE - 1) /
		   DWMMC_DMA_MAX_BUFFER_SIZE;
	assert(desc_cnt * sizeof(struct dw_idmac_desc) < dw_params.desc_size);

	base = dw_params.reg_base;
	desc = (struct dw_idmac_desc *)dw_params.desc_base;
	for (i = 0; i < desc_cnt; i++) {
		desc[i].des0 = IDMAC_DES0_OWN | IDMAC_DES0_CH | IDMAC_DES0_DIC;
		desc[i].des1 = IDMAC_DES1_BS1(DWMMC_DMA_MAX_BUFFER_SIZE);
		desc[i].des2 = buf + DWMMC_DMA_MAX_BUFFER_SIZE * i;
		desc[i].des3 = dw_params.desc_base +
			       (sizeof(struct dw_idmac_desc)) * (i + 1);
	}
	/* first descriptor */
	desc->des0 |= IDMAC_DES0_FS;
	/* last descriptor */
	last = desc_cnt - 1;
	(desc + last)->des0 |= IDMAC_DES0_LD;
	(desc + last)->des0 &= ~(IDMAC_DES0_DIC | IDMAC_DES0_CH);
	(desc + last)->des1 = IDMAC_DES1_BS1(size - (last *
				  DWMMC_DMA_MAX_BUFFER_SIZE));
	/* set next descriptor address as 0 */
	(desc + last)->des3 = 0;

	mmio_write_32(base + DWMMC_DBADDR, dw_params.desc_base);
	clean_dcache_range(dw_params.desc_base,
			   desc_cnt * DWMMC_DMA_MAX_BUFFER_SIZE);
#endif
}

static int dw_prepare(int lba, uintptr_t buf, size_t size)
{
	uintptr_t base = dw_params.reg_base;

	assert(((buf & EMMC_BLOCK_MASK) == 0) &&
	       ((size % EMMC_BLOCK_SIZE) == 0));

	mmio_write_32(base + DWMMC_BYTCNT, size);
	mmio_write_32(base + DWMMC_RINTSTS, ~0);
	init_dma(buf, size);

#ifdef DWMMC_NO_DMA
	/*
	 * We can't handle more data than the FIFO can hold because
	 * send_cmd() assumes it can just wait for the command to
	 * complete, but we would need to restructure the code to keep
	 * reading to, or writing from, the FIFO to make that happen.
	 */
	if (size > dw_fifo_depth)
		return -EINVAL;
#endif

	return 0;
}

static int dw_read(int lba, uintptr_t buf, size_t size)
{
#ifdef DWMMC_NO_DMA
	uint32_t *p = (uint32_t *)buf;
	uintptr_t base = dw_params.reg_base;

	assert((size % 4) == 0);

	for (; size; size -= 4, ++p)
		*p = mmio_read_32(base + DWMMC_FIFO);
#endif

	return 0;
}

static int dw_write(int lba, uintptr_t buf, size_t size)
{
	return 0;
}

void dw_mmc_init(dw_mmc_params_t *params)
{
	assert((params != 0) &&
	       ((params->reg_base & EMMC_BLOCK_MASK) == 0) &&
	       ((params->desc_base & EMMC_BLOCK_MASK) == 0) &&
	       ((params->desc_size & EMMC_BLOCK_MASK) == 0) &&
	       (params->desc_size > 0) &&
	       (params->clk_rate > 0) &&
	       ((params->bus_width == EMMC_BUS_WIDTH_1) ||
		(params->bus_width == EMMC_BUS_WIDTH_4) ||
		(params->bus_width == EMMC_BUS_WIDTH_8)));

	memcpy(&dw_params, params, sizeof(dw_mmc_params_t));
	emmc_init(&dw_mmc_ops, params->clk_rate, params->bus_width,
		  params->flags);
}
