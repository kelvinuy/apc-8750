/*
 *  linux/drivers/mmc/sdio_ops.c
 *
 *  Copyright 2006-2007 Pierre Ossman
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#include <linux/scatterlist.h>

#include <linux/mmc/host.h>
#include <linux/mmc/card.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/sdio.h>

#include "core.h"
#include "sdio_ops.h"

#if 0
#define DBG(x...)	printk(KERN_ALERT x)
#else
#define DBG(x...)	do { } while (0)
#endif

int mmc_send_io_op_cond(struct mmc_host *host, u32 ocr, u32 *rocr)
{
	struct mmc_command cmd;
	int i, err = 0;
	DBG("[%s] s\n",__func__);
	
	BUG_ON(!host);

	memset(&cmd, 0, sizeof(struct mmc_command));

	cmd.opcode = SD_IO_SEND_OP_COND;
	cmd.arg = ocr;
	cmd.flags = MMC_RSP_SPI_R4 | MMC_RSP_R4 | MMC_CMD_BCR;
	//cmd.flags = MMC_RSP_R4 | MMC_CMD_BCR;	//zhf: mark SPI mode temporarily by James Tian

	for (i = 100; i; i--) {
		err = mmc_wait_for_cmd(host, &cmd, MMC_CMD_RETRIES);
		if (err)
			break;

		/* if we're just probing, do a single pass */
		if (ocr == 0)
			break;

		/* otherwise wait until reset completes */
		if (mmc_host_is_spi(host)) {
			/*
			 * Both R1_SPI_IDLE and MMC_CARD_BUSY indicate
			 * an initialized card under SPI, but some cards
			 * (Marvell's) only behave when looking at this
			 * one.
			 */
			if (cmd.resp[1] & MMC_CARD_BUSY)
				break;
		} else {
			if (cmd.resp[0] & MMC_CARD_BUSY)
				break;
		}

		err = -ETIMEDOUT;

		mmc_delay(10);
	}

	if (rocr)
		*rocr = cmd.resp[mmc_host_is_spi(host) ? 1 : 0];

	DBG("[%s] e\n",__func__);
	return err;
}

int mmc_io_rw_direct(struct mmc_card *card, int write, unsigned fn,
	unsigned addr, u8 in, u8* out)
{
	struct mmc_command cmd;
	int err;
	DBG("[%s] s\n",__func__);
	
	BUG_ON(!card);
	BUG_ON(fn > 7);

	/* sanity check */
	if (addr & ~0x1FFFF)
		return -EINVAL;

	memset(&cmd, 0, sizeof(struct mmc_command));

	cmd.opcode = SD_IO_RW_DIRECT;
	cmd.arg = write ? 0x80000000 : 0x00000000;
	cmd.arg |= fn << 28;
	cmd.arg |= (write && out) ? 0x08000000 : 0x00000000;
	cmd.arg |= addr << 9;
	cmd.arg |= in;
	cmd.flags = MMC_RSP_SPI_R5 | MMC_RSP_R5 | MMC_CMD_AC;
	//cmd.flags = MMC_RSP_R5 | MMC_CMD_AC;	//zhf: mark SPI mode temporarily

	err = mmc_wait_for_cmd(card->host, &cmd, 0);
	if (err) {
		DBG("[%s] e1\n",__func__);
		return err;
	}

	if (mmc_host_is_spi(card->host)) {
		/* host driver already reported errors */
	} else {
		if (cmd.resp[0] & R5_ERROR) {
			DBG("[%s] e2\n",__func__);
			return -EIO;
		}
		if (cmd.resp[0] & R5_FUNCTION_NUMBER) {
			DBG("[%s] e3\n",__func__);
			return -EINVAL;
		}
		if (cmd.resp[0] & R5_OUT_OF_RANGE) {
			DBG("[%s] e4\n",__func__);
			return -ERANGE;
		}
	}

	if (out) {
		if (mmc_host_is_spi(card->host))
			*out = (cmd.resp[0] >> 8) & 0xFF;
		else
			*out = cmd.resp[0] & 0xFF;
	}
	DBG("[%s] e5\n",__func__);
	return 0;
}

int mmc_io_rw_extended(struct mmc_card *card, int write, unsigned fn,
	unsigned addr, int incr_addr, u8 *buf, unsigned blocks, unsigned blksz)
{
	struct mmc_request mrq;
	struct mmc_command cmd;
	struct mmc_data data;
	struct scatterlist sg;
	DBG("[%s] s\n",__func__);

	BUG_ON(!card);
	BUG_ON(fn > 7);
	BUG_ON(blocks == 1 && blksz > 512);
	WARN_ON(blocks == 0);
	WARN_ON(blksz == 0);

	/* sanity check */
	if (addr & ~0x1FFFF)
		return -EINVAL;

	memset(&mrq, 0, sizeof(struct mmc_request));
	memset(&cmd, 0, sizeof(struct mmc_command));
	memset(&data, 0, sizeof(struct mmc_data));

	mrq.cmd = &cmd;
	mrq.data = &data;

	cmd.opcode = SD_IO_RW_EXTENDED;
	cmd.arg = write ? 0x80000000 : 0x00000000;
	cmd.arg |= fn << 28;
	cmd.arg |= incr_addr ? 0x04000000 : 0x00000000;
	cmd.arg |= addr << 9;
	if (blocks == 1 && blksz <= 512)
		cmd.arg |= (blksz == 512) ? 0 : blksz;	/* byte mode */
	else
		cmd.arg |= 0x08000000 | blocks;		/* block mode */
	cmd.flags = MMC_RSP_SPI_R5 | MMC_RSP_R5 | MMC_CMD_ADTC;
	//cmd.flags = MMC_RSP_R5 | MMC_CMD_ADTC;	//zhf: mark SPI mode temporarily

	data.blksz = blksz;
	data.blocks = blocks;
	data.flags = write ? MMC_DATA_WRITE : MMC_DATA_READ;
	data.sg = &sg;
	data.sg_len = 1;

	sg_init_one(&sg, buf, blksz * blocks);

	mmc_set_data_timeout(&data, card);

	mmc_wait_for_req(card->host, &mrq);

	if (cmd.error) {
		DBG("[%s] e1\n",__func__);
		return cmd.error;
	}
	if (data.error) {
		DBG("[%s] e2\n",__func__);
		return data.error;
	}

	if (mmc_host_is_spi(card->host)) {
		/* host driver already reported errors */
	} else {
		if (cmd.resp[0] & R5_ERROR) {
			DBG("[%s] e3\n",__func__);
			return -EIO;
		}
		if (cmd.resp[0] & R5_FUNCTION_NUMBER) {
			DBG("[%s] e4\n",__func__);
			return -EINVAL;
		}
		if (cmd.resp[0] & R5_OUT_OF_RANGE) {
			DBG("[%s] e5\n",__func__);
			return -ERANGE;
		}
	}

	DBG("[%s] e6\n",__func__);
	return 0;
}

