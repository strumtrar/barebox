// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2015 Pengutronix, Steffen Trumtrar <s.trumtrar@pengutronix.de>
 *
 * derived from Linux kernel driver by
 *
 * Driver for Cadence QSPI Controller
 *
 * Copyright Altera Corporation (C) 2012-2014. All rights reserved.
 */

#include <clock.h>
#include <common.h>
#include <driver.h>
#include <errno.h>
#include <init.h>
#include <io.h>
#include <linux/clk.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/spi-nor.h>
#include <of.h>
#include <platform_data/cadence_qspi.h>
#include <spi/spi.h>

#define CQSPI_MAX_CHIPSELECT		16

struct cqspi_flash_pdata {
	struct mtd_info mtd;
	struct spi_nor nor;
	u32 clk_rate;
	unsigned int block_size;
	unsigned int read_delay;
	unsigned int tshsl_ns;
	unsigned int tsd2d_ns;
	unsigned int tchsh_ns;
	unsigned int tslch_ns;
	u8	     inst_width;
	u8	     addr_width;
	u8	     data_width;
};

struct cqspi_st {
	struct device	*dev;
	struct clk	*l4_mp_clk;
	struct clk	*qspi_clk;
	unsigned int	sclk;

	void __iomem	*iobase;
	void __iomem	*ahb_base;
	unsigned int	irq_mask;
	int		current_cs;
	unsigned int	master_ref_clk_hz;
	unsigned int	is_decoded_cs;
	unsigned int	fifo_depth;
	struct cqspi_flash_pdata f_pdata[CQSPI_MAX_CHIPSELECT];
	bool no_reconfig;
};

/* Operation timeout value */
#define CQSPI_TIMEOUT_MS			800 * MSECOND
#define CQSPI_READ_TIMEOUT_MS			100 * MSECOND
#define CQSPI_POLL_IDLE_RETRY			3

#define CQSPI_FIFO_WIDTH			4

#define CQSPI_REG_SRAM_THRESHOLD_BYTES		50

/* Instruction type */
#define CQSPI_INST_TYPE_SINGLE			0
#define CQSPI_INST_TYPE_DUAL			1
#define CQSPI_INST_TYPE_QUAD			2

#define CQSPI_DUMMY_CLKS_PER_BYTE		8
#define CQSPI_DUMMY_BYTES_MAX			4

#define CQSPI_STIG_DATA_LEN_MAX			8

#define CQSPI_INDIRECTTRIGGER_ADDR_MASK		0xFFFFF

/* Register map */
#define CQSPI_REG_CONFIG			0x00
#define CQSPI_REG_CONFIG_ENABLE_MASK		BIT(0)
#define CQSPI_REG_CONFIG_DECODE_MASK		BIT(9)
#define CQSPI_REG_CONFIG_CHIPSELECT_LSB		10
#define CQSPI_REG_CONFIG_DMA_MASK		BIT(15)
#define CQSPI_REG_CONFIG_BAUD_LSB		19
#define CQSPI_REG_CONFIG_IDLE_LSB		31
#define CQSPI_REG_CONFIG_CHIPSELECT_MASK	0xF
#define CQSPI_REG_CONFIG_BAUD_MASK		0xF

#define CQSPI_REG_RD_INSTR			0x04
#define CQSPI_REG_RD_INSTR_OPCODE_LSB		0
#define CQSPI_REG_RD_INSTR_TYPE_INSTR_LSB	8
#define CQSPI_REG_RD_INSTR_TYPE_ADDR_LSB	12
#define CQSPI_REG_RD_INSTR_TYPE_DATA_LSB	16
#define CQSPI_REG_RD_INSTR_MODE_EN_LSB		20
#define CQSPI_REG_RD_INSTR_DUMMY_LSB		24
#define CQSPI_REG_RD_INSTR_TYPE_INSTR_MASK	0x3
#define CQSPI_REG_RD_INSTR_TYPE_ADDR_MASK	0x3
#define CQSPI_REG_RD_INSTR_TYPE_DATA_MASK	0x3
#define CQSPI_REG_RD_INSTR_DUMMY_MASK		0x1F

#define CQSPI_REG_WR_INSTR			0x08
#define CQSPI_REG_WR_INSTR_OPCODE_LSB		0
#define CQSPI_REG_WR_INSTR_TYPE_ADDR_LSB	12
#define CQSPI_REG_WR_INSTR_TYPE_DATA_LSB	16

#define CQSPI_REG_DELAY				0x0C
#define CQSPI_REG_DELAY_TSLCH_LSB		0
#define CQSPI_REG_DELAY_TCHSH_LSB		8
#define CQSPI_REG_DELAY_TSD2D_LSB		16
#define CQSPI_REG_DELAY_TSHSL_LSB		24
#define CQSPI_REG_DELAY_TSLCH_MASK		0xFF
#define CQSPI_REG_DELAY_TCHSH_MASK		0xFF
#define CQSPI_REG_DELAY_TSD2D_MASK		0xFF
#define CQSPI_REG_DELAY_TSHSL_MASK		0xFF

#define CQSPI_REG_READCAPTURE			0x10
#define CQSPI_REG_READCAPTURE_BYPASS_LSB	0
#define CQSPI_REG_READCAPTURE_DELAY_LSB		1
#define CQSPI_REG_READCAPTURE_DELAY_MASK	0xF

#define CQSPI_REG_SIZE				0x14
#define CQSPI_REG_SIZE_ADDRESS_LSB		0
#define CQSPI_REG_SIZE_PAGE_LSB			4
#define CQSPI_REG_SIZE_BLOCK_LSB		16
#define CQSPI_REG_SIZE_ADDRESS_MASK		0xF
#define CQSPI_REG_SIZE_PAGE_MASK		0xFFF
#define CQSPI_REG_SIZE_BLOCK_MASK		0x3F

#define CQSPI_REG_SRAMPARTITION			0x18
#define CQSPI_REG_INDIRECTTRIGGER		0x1C

#define CQSPI_REG_DMA				0x20
#define CQSPI_REG_DMA_SINGLE_LSB		0
#define CQSPI_REG_DMA_BURST_LSB			8
#define CQSPI_REG_DMA_SINGLE_MASK		0xFF
#define CQSPI_REG_DMA_BURST_MASK		0xFF

#define CQSPI_REG_REMAP				0x24
#define CQSPI_REG_MODE_BIT			0x28

#define CQSPI_REG_SRAMLEVEL			0x2C
#define CQSPI_REG_SRAMLEVEL_RD_LSB		0
#define CQSPI_REG_SRAMLEVEL_WR_LSB		16
#define CQSPI_REG_SRAMLEVEL_RD_MASK		0xFFFF
#define CQSPI_REG_SRAMLEVEL_WR_MASK		0xFFFF

#define CQSPI_REG_IRQSTATUS			0x40
#define CQSPI_REG_IRQMASK			0x44

#define CQSPI_REG_INDIRECTRD			0x60
#define CQSPI_REG_INDIRECTRD_START_MASK		BIT(0)
#define CQSPI_REG_INDIRECTRD_CANCEL_MASK	BIT(1)
#define CQSPI_REG_INDIRECTRD_DONE_MASK		BIT(5)

#define CQSPI_REG_INDIRECTRDWATERMARK		0x64
#define CQSPI_REG_INDIRECTRDSTARTADDR		0x68
#define CQSPI_REG_INDIRECTRDBYTES		0x6C

#define CQSPI_REG_CMDCTRL			0x90
#define CQSPI_REG_CMDCTRL_EXECUTE_MASK		BIT(0)
#define CQSPI_REG_CMDCTRL_INPROGRESS_MASK	BIT(1)
#define CQSPI_REG_CMDCTRL_WR_BYTES_LSB		12
#define CQSPI_REG_CMDCTRL_WR_EN_LSB		15
#define CQSPI_REG_CMDCTRL_ADD_BYTES_LSB		16
#define CQSPI_REG_CMDCTRL_ADDR_EN_LSB		19
#define CQSPI_REG_CMDCTRL_RD_BYTES_LSB		20
#define CQSPI_REG_CMDCTRL_RD_EN_LSB		23
#define CQSPI_REG_CMDCTRL_OPCODE_LSB		24
#define CQSPI_REG_CMDCTRL_WR_BYTES_MASK		0x7
#define CQSPI_REG_CMDCTRL_ADD_BYTES_MASK	0x3
#define CQSPI_REG_CMDCTRL_RD_BYTES_MASK		0x7

#define CQSPI_REG_INDIRECTWR			0x70
#define CQSPI_REG_INDIRECTWR_START_MASK		BIT(0)
#define CQSPI_REG_INDIRECTWR_CANCEL_MASK	BIT(1)
#define CQSPI_REG_INDIRECTWR_DONE_MASK		BIT(5)

#define CQSPI_REG_INDIRECTWRWATERMARK		0x74
#define CQSPI_REG_INDIRECTWRSTARTADDR		0x78
#define CQSPI_REG_INDIRECTWRBYTES		0x7C

#define CQSPI_REG_CMDADDRESS			0x94
#define CQSPI_REG_CMDREADDATALOWER		0xA0
#define CQSPI_REG_CMDREADDATAUPPER		0xA4
#define CQSPI_REG_CMDWRITEDATALOWER		0xA8
#define CQSPI_REG_CMDWRITEDATAUPPER		0xAC

/* Interrupt status bits */
#define CQSPI_REG_IRQ_MODE_ERR			BIT(0)
#define CQSPI_REG_IRQ_UNDERFLOW			BIT(1)
#define CQSPI_REG_IRQ_IND_COMP			BIT(2)
#define CQSPI_REG_IRQ_IND_RD_REJECT		BIT(3)
#define CQSPI_REG_IRQ_WR_PROTECTED_ERR		BIT(4)
#define CQSPI_REG_IRQ_ILLEGAL_AHB_ERR		BIT(5)
#define CQSPI_REG_IRQ_WATERMARK			BIT(6)
#define CQSPI_REG_IRQ_IND_RD_OVERFLOW		BIT(12)

#define CQSPI_IRQ_STATUS_ERR		(CQSPI_REG_IRQ_MODE_ERR		| \
					 CQSPI_REG_IRQ_IND_RD_REJECT	| \
					 CQSPI_REG_IRQ_WR_PROTECTED_ERR	| \
					 CQSPI_REG_IRQ_ILLEGAL_AHB_ERR)

#define CQSPI_IRQ_MASK_RD		(CQSPI_REG_IRQ_WATERMARK	| \
					 CQSPI_REG_IRQ_IND_RD_OVERFLOW	| \
					 CQSPI_REG_IRQ_IND_COMP)

#define CQSPI_IRQ_MASK_WR		(CQSPI_REG_IRQ_IND_COMP		| \
					 CQSPI_REG_IRQ_WATERMARK	| \
					 CQSPI_REG_IRQ_UNDERFLOW)

#define CQSPI_IRQ_STATUS_MASK		0x1FFFE

#define CQSPI_REG_IS_IDLE(base)						\
		((readl(base + CQSPI_REG_CONFIG) >>			\
			CQSPI_REG_CONFIG_IDLE_LSB) & 0x1)

#define CQSPI_GET_RD_SRAM_LEVEL(reg_base)				\
		(((readl(reg_base + CQSPI_REG_SRAMLEVEL)) >>		\
		CQSPI_REG_SRAMLEVEL_RD_LSB) & CQSPI_REG_SRAMLEVEL_RD_MASK)

static void cqspi_fifo_read(void *dest, const void *src_ahb_addr,
			    unsigned int bytes)
{
	unsigned int temp;
	int remaining = bytes;
	unsigned int *dest_ptr = (unsigned int *)dest;
	unsigned int *src_ptr = (unsigned int *)src_ahb_addr;

	while (remaining > CQSPI_FIFO_WIDTH) {
		*dest_ptr = readl(src_ptr);
		dest_ptr++;
		remaining -= CQSPI_FIFO_WIDTH;
	}
	if (remaining > 0) {
		/* dangling bytes */
		temp = readl(src_ptr);
		memcpy(dest_ptr, &temp, remaining);
	}
}

static void cqspi_fifo_write(void *dest_ahb_addr,
			     const void *src, unsigned int bytes)
{
	unsigned int temp;
	int remaining = bytes;
	unsigned int *dest_ptr = (unsigned int *)dest_ahb_addr;
	unsigned int *src_ptr = (unsigned int *)src;

	while (remaining > CQSPI_FIFO_WIDTH) {
		writel(*src_ptr, dest_ptr);
		src_ptr++;
		remaining -= CQSPI_FIFO_WIDTH;
	}
	if (remaining > 0) {
		/* dangling bytes */
		memcpy(&temp, src_ptr, remaining);
		writel(temp, dest_ptr);
	}
}

static int cqspi_find_chipselect(struct spi_nor *nor)
{
	int cs = -1;
	struct cqspi_st *cqspi = nor->priv;

	for (cs = 0; cs < CQSPI_MAX_CHIPSELECT; cs++)
		if (nor == &cqspi->f_pdata[cs].nor)
			break;
	return cs;
}

static unsigned int cqspi_calc_rdreg(struct spi_nor *nor, u8 opcode)
{
	unsigned int rdreg = 0;
	struct cqspi_st *cqspi = nor->priv;
	struct cqspi_flash_pdata *f_pdata;

	f_pdata = &cqspi->f_pdata[cqspi->current_cs];

	rdreg |= f_pdata->inst_width << CQSPI_REG_RD_INSTR_TYPE_INSTR_LSB;
	rdreg |= f_pdata->addr_width << CQSPI_REG_RD_INSTR_TYPE_ADDR_LSB;
	rdreg |= f_pdata->data_width << CQSPI_REG_RD_INSTR_TYPE_DATA_LSB;

	return rdreg;
}

static unsigned int cqspi_wait_idle(struct cqspi_st *cqspi)
{
	void __iomem *reg_base = cqspi->iobase;

	if (wait_on_timeout(CQSPI_TIMEOUT_MS,
		CQSPI_REG_IS_IDLE(reg_base))) {
		/* Timeout, in busy mode. */
		dev_err(cqspi->dev, "QSPI is still busy after %llums timeout.\n",
			CQSPI_TIMEOUT_MS);
		return -ETIMEDOUT;
	}

	return 0;
}

static int cqspi_exec_flash_cmd(struct cqspi_st *cqspi, unsigned int reg)
{
	void __iomem *reg_base = cqspi->iobase;

	/* Write the CMDCTRL without start execution. */
	writel(reg, reg_base + CQSPI_REG_CMDCTRL);
	/* Start execute */
	reg |= CQSPI_REG_CMDCTRL_EXECUTE_MASK;
	writel(reg, reg_base + CQSPI_REG_CMDCTRL);

	if (wait_on_timeout(CQSPI_TIMEOUT_MS,
		(readl(reg_base + CQSPI_REG_CMDCTRL) &
			CQSPI_REG_CMDCTRL_INPROGRESS_MASK) == 0)) {
		dev_err(cqspi->dev, "flash cmd execute time out (0x%08x)\n",
			readl(reg_base + CQSPI_REG_CMDCTRL));
		return -EIO;
	}

	/* Polling QSPI idle status. */
	return cqspi_wait_idle(cqspi);
}

static int cqspi_command_read(struct spi_nor *nor,
			      const u8 *txbuf, unsigned n_tx,
			      u8 *rxbuf, unsigned n_rx)
{
	unsigned int reg;
	unsigned int read_len;
	int status;
	struct cqspi_st *cqspi = nor->priv;
	void __iomem *reg_base = cqspi->iobase;
	unsigned int rdreg;

	if (!n_rx || n_rx > CQSPI_STIG_DATA_LEN_MAX || rxbuf == NULL) {
		dev_err(nor->dev,
			"Invalid input argument, len %d rxbuf 0x%p\n", n_rx, rxbuf);
		return -EINVAL;
	}

	reg = txbuf[0] << CQSPI_REG_CMDCTRL_OPCODE_LSB;

	rdreg = cqspi_calc_rdreg(nor, txbuf[0]);
	writel(rdreg, reg_base + CQSPI_REG_RD_INSTR);

	reg |= (0x1 << CQSPI_REG_CMDCTRL_RD_EN_LSB);

	/* 0 means 1 byte. */
	reg |= (((n_rx - 1) & CQSPI_REG_CMDCTRL_RD_BYTES_MASK)
		<< CQSPI_REG_CMDCTRL_RD_BYTES_LSB);
	status = cqspi_exec_flash_cmd(cqspi, reg);
	if (status != 0)
		return status;

	reg = readl(reg_base + CQSPI_REG_CMDREADDATALOWER);

	/* Put the read value into rx_buf */
	read_len = (n_rx > 4) ? 4 : n_rx;
	memcpy(rxbuf, &reg, read_len);
	rxbuf += read_len;

	if (n_rx > 4) {
		reg = readl(reg_base + CQSPI_REG_CMDREADDATAUPPER);

		read_len = n_rx - read_len;
		memcpy(rxbuf, &reg, read_len);
	}

	return 0;
}

static __maybe_unused int cqspi_command_write(struct spi_nor *nor,
				u8 opcode, const u8 *txbuf, unsigned n_tx)
{
	unsigned int reg;
	unsigned int data;
	struct cqspi_st *cqspi = nor->priv;
	void __iomem *reg_base = cqspi->iobase;

	if (n_tx > 4 || (n_tx && txbuf == NULL)) {
		dev_err(nor->dev,
			"Invalid input argument, cmdlen %d txbuf 0x%p\n", n_tx, txbuf);
		return -EINVAL;
	}

	reg = opcode << CQSPI_REG_CMDCTRL_OPCODE_LSB;
	if (n_tx) {
		reg |= (0x1 << CQSPI_REG_CMDCTRL_WR_EN_LSB);
		reg |= ((n_tx - 1) & CQSPI_REG_CMDCTRL_WR_BYTES_MASK)
		    << CQSPI_REG_CMDCTRL_WR_BYTES_LSB;
		data = 0;
		memcpy(&data, txbuf, n_tx);
		writel(data, reg_base + CQSPI_REG_CMDWRITEDATALOWER);
	}

	return cqspi_exec_flash_cmd(cqspi, reg);
}

static int cqspi_command_write_addr(struct spi_nor *nor,
				    u8 opcode, unsigned int addr)
{
	unsigned int reg;
	struct cqspi_st *cqspi = nor->priv;
	void __iomem *reg_base = cqspi->iobase;

	reg = opcode << CQSPI_REG_CMDCTRL_OPCODE_LSB;
	reg |= (0x1 << CQSPI_REG_CMDCTRL_ADDR_EN_LSB);
	reg |= ((nor->addr_width - 1) & CQSPI_REG_CMDCTRL_ADD_BYTES_MASK)
		<< CQSPI_REG_CMDCTRL_ADD_BYTES_LSB;

	writel(addr, reg_base + CQSPI_REG_CMDADDRESS);

	return cqspi_exec_flash_cmd(cqspi, reg);
}

static int cqspi_indirect_read_setup(struct spi_nor *nor,
				     unsigned int from_addr)
{
	struct cqspi_flash_pdata *f_pdata;
	struct cqspi_st *cqspi = nor->priv;
	phys_addr_t ahb_base = virt_to_phys(cqspi->ahb_base);
	void __iomem *reg_base = cqspi->iobase;
	unsigned int dummy_clk = 0;
	unsigned int dummy_bytes;
	unsigned int reg = 0;

	writel(ahb_base & CQSPI_INDIRECTTRIGGER_ADDR_MASK,
	       reg_base + CQSPI_REG_INDIRECTTRIGGER);
	writel(from_addr, reg_base + CQSPI_REG_INDIRECTRDSTARTADDR);
	f_pdata = &cqspi->f_pdata[cqspi->current_cs];

	reg = nor->read_opcode << CQSPI_REG_RD_INSTR_OPCODE_LSB;
	reg |= cqspi_calc_rdreg(nor, nor->read_opcode);

	/* Setup dummy clock cycles */
	dummy_bytes = nor->read_dummy / 8;

	if (dummy_bytes > CQSPI_DUMMY_BYTES_MAX)
		dummy_bytes = CQSPI_DUMMY_BYTES_MAX;

	if (dummy_bytes) {
		reg |= (1 << CQSPI_REG_RD_INSTR_MODE_EN_LSB);
		/* Set mode bits high to ensure chip doesn't enter XIP */
		writel(0xFF, reg_base + CQSPI_REG_MODE_BIT);

		/* Convert to clock cycles. */
		dummy_clk = dummy_bytes * CQSPI_DUMMY_CLKS_PER_BYTE;
		/* Need to subtract the mode byte (8 clocks). */
		if (f_pdata->inst_width != CQSPI_INST_TYPE_QUAD)
			dummy_clk -= CQSPI_DUMMY_CLKS_PER_BYTE;

		if (dummy_clk)
			reg |= (dummy_clk & CQSPI_REG_RD_INSTR_DUMMY_MASK)
			    << CQSPI_REG_RD_INSTR_DUMMY_LSB;
	}

	writel(reg, reg_base + CQSPI_REG_RD_INSTR);

	/* Set address width */
	reg = readl(reg_base + CQSPI_REG_SIZE);
	reg &= ~CQSPI_REG_SIZE_ADDRESS_MASK;
	reg |= (nor->addr_width) - 1;
	writel(reg, reg_base + CQSPI_REG_SIZE);
	return 0;
}

static int cqspi_indirect_read_execute(struct spi_nor *nor,
				       u8 *rxbuf, unsigned n_rx)
{
	int ret = 0;
	unsigned int reg = 0;
	unsigned int bytes_to_read = 0;
	unsigned int watermark;
	struct cqspi_st *cqspi = nor->priv;
	void __iomem *reg_base = cqspi->iobase;
	void __iomem *ahb_base = cqspi->ahb_base;
	int remaining = (int)n_rx;

	watermark = cqspi->fifo_depth * CQSPI_FIFO_WIDTH / 2;
	writel(watermark, reg_base + CQSPI_REG_INDIRECTRDWATERMARK);
	writel(remaining, reg_base + CQSPI_REG_INDIRECTRDBYTES);

	/* Clear all interrupts. */
	writel(CQSPI_IRQ_STATUS_MASK, reg_base + CQSPI_REG_IRQSTATUS);

	cqspi->irq_mask = CQSPI_IRQ_MASK_RD;
	writel(cqspi->irq_mask, reg_base + CQSPI_REG_IRQMASK);

	writel(CQSPI_REG_INDIRECTRD_START_MASK,
		reg_base + CQSPI_REG_INDIRECTRD);

	while (remaining > 0) {
		unsigned int irq_status;

		ret = wait_on_timeout(CQSPI_READ_TIMEOUT_MS,
			readl(reg_base + CQSPI_REG_IRQSTATUS) & cqspi->irq_mask);

		irq_status = readl(reg_base + CQSPI_REG_IRQSTATUS);
		bytes_to_read = CQSPI_GET_RD_SRAM_LEVEL(reg_base);

		/* Clear all interrupts. */
		writel(irq_status, reg_base + CQSPI_REG_IRQSTATUS);

		if (!ret && bytes_to_read == 0) {
			dev_err(nor->dev, "Indirect read timeout, no bytes\n");
			ret = -ETIMEDOUT;
			goto failrd;
		}

		while (bytes_to_read != 0) {
			bytes_to_read *= CQSPI_FIFO_WIDTH;
			bytes_to_read = bytes_to_read > remaining
					? remaining : bytes_to_read;
			cqspi_fifo_read(rxbuf, ahb_base, bytes_to_read);
			rxbuf += bytes_to_read;
			remaining -= bytes_to_read;
			bytes_to_read = CQSPI_GET_RD_SRAM_LEVEL(reg_base);
		}
	}

	/* Check indirect done status */
	if (wait_on_timeout(CQSPI_TIMEOUT_MS,
		readl(reg_base + CQSPI_REG_INDIRECTRD) &
			CQSPI_REG_INDIRECTRD_DONE_MASK)) {
		dev_err(nor->dev,
			"Indirect read completion error 0x%08x\n", reg);
		ret = -ETIMEDOUT;
		goto failrd;
	}

	/* Disable interrupt */
	writel(0, reg_base + CQSPI_REG_IRQMASK);

	/* Clear indirect completion status */
	writel(CQSPI_REG_INDIRECTRD_DONE_MASK, reg_base + CQSPI_REG_INDIRECTRD);

	return 0;

 failrd:
	/* Disable interrupt */
	writel(0, reg_base + CQSPI_REG_IRQMASK);

	/* Cancel the indirect read */
	writel(CQSPI_REG_INDIRECTWR_CANCEL_MASK,
	       reg_base + CQSPI_REG_INDIRECTRD);
	return ret;
}

static __maybe_unused int cqspi_indirect_write_setup(struct spi_nor *nor,
						     unsigned int to_addr)
{
	unsigned int reg;
	struct cqspi_st *cqspi = nor->priv;
	void __iomem *reg_base = cqspi->iobase;

	/* Set opcode. */
	reg = nor->program_opcode << CQSPI_REG_WR_INSTR_OPCODE_LSB;
	writel(reg, reg_base + CQSPI_REG_WR_INSTR);
	reg = cqspi_calc_rdreg(nor, nor->program_opcode);
	writel(reg, reg_base + CQSPI_REG_RD_INSTR);

	writel(to_addr, reg_base + CQSPI_REG_INDIRECTWRSTARTADDR);

	reg = readl(reg_base + CQSPI_REG_SIZE);
	reg &= ~CQSPI_REG_SIZE_ADDRESS_MASK;
	reg |= (nor->addr_width - 1);
	writel(reg, reg_base + CQSPI_REG_SIZE);
	return 0;
}

static __maybe_unused int cqspi_indirect_write_execute(struct spi_nor *nor,
						const u8 *txbuf, unsigned n_tx)
{
	int ret;
	unsigned int reg = 0;
	struct cqspi_st *cqspi = nor->priv;
	void __iomem *reg_base = cqspi->iobase;
	void __iomem *ahb_base = cqspi->ahb_base;
	int remaining = (int)n_tx;
	unsigned int page_size;
	unsigned int write_bytes;

	page_size = nor->page_size;

	writel(remaining, reg_base + CQSPI_REG_INDIRECTWRBYTES);

	writel(CQSPI_REG_SRAM_THRESHOLD_BYTES, reg_base +
	       CQSPI_REG_INDIRECTWRWATERMARK);

	/* Clear all interrupts. */
	writel(CQSPI_IRQ_STATUS_MASK, reg_base + CQSPI_REG_IRQSTATUS);

	cqspi->irq_mask = CQSPI_IRQ_MASK_WR;
	writel(cqspi->irq_mask, reg_base + CQSPI_REG_IRQMASK);

	writel(CQSPI_REG_INDIRECTWR_START_MASK,
	       reg_base + CQSPI_REG_INDIRECTWR);

	/* Write a page or remaining bytes. */
	write_bytes = remaining > page_size ? page_size : remaining;
	/* Fill up the data at the beginning */
	cqspi_fifo_write(ahb_base, txbuf, write_bytes);
	txbuf += write_bytes;
	remaining -= write_bytes;

	while (remaining > 0) {
		ret = wait_on_timeout(CQSPI_READ_TIMEOUT_MS,
			readl(reg_base + CQSPI_REG_IRQSTATUS) & cqspi->irq_mask);

		/* Clear all interrupts. */
		writel(CQSPI_IRQ_STATUS_MASK, reg_base + CQSPI_REG_IRQSTATUS);

		if (ret < 0) {
			dev_err(nor->dev, "Indirect write timeout\n");
			ret = -ETIMEDOUT;
			goto failwr;
		}

		write_bytes = remaining > page_size ? page_size : remaining;
		cqspi_fifo_write(ahb_base, txbuf, write_bytes);
		txbuf += write_bytes;
		remaining -= write_bytes;

		writel(cqspi->irq_mask, reg_base + CQSPI_REG_IRQMASK);
	}
	ret = wait_on_timeout(CQSPI_READ_TIMEOUT_MS,
			readl(reg_base + CQSPI_REG_IRQSTATUS) & cqspi->irq_mask);

	/* Clear all interrupts. */
	writel(CQSPI_IRQ_STATUS_MASK, reg_base + CQSPI_REG_IRQSTATUS);
	if (ret < 0) {
		dev_err(nor->dev, "Indirect write timeout\n");
		ret = -ETIMEDOUT;
		goto failwr;
	}

	/* Check indirect done status */
	if (wait_on_timeout(CQSPI_TIMEOUT_MS,
		readl(reg_base + CQSPI_REG_INDIRECTWR)
			& CQSPI_REG_INDIRECTWR_DONE_MASK)) {
		dev_err(nor->dev,
			"Indirect write completion error 0x%08x\n", reg);
		ret = -ETIMEDOUT;
		goto failwr;
	}

	/* Disable interrupt. */
	writel(0, reg_base + CQSPI_REG_IRQMASK);

	/* Clear indirect completion status */
	writel(CQSPI_REG_INDIRECTWR_DONE_MASK, reg_base + CQSPI_REG_INDIRECTWR);

	cqspi_wait_idle(cqspi);

	return 0;

failwr:
	/* Disable interrupt. */
	writel(0, reg_base + CQSPI_REG_IRQMASK);

	/* Cancel the indirect write */
	writel(CQSPI_REG_INDIRECTWR_CANCEL_MASK,
	       reg_base + CQSPI_REG_INDIRECTWR);
	return ret;
}

static void cqspi_controller_enable(struct cqspi_st *cqspi)
{
	void __iomem *reg_base = cqspi->iobase;
	unsigned int reg;

	reg = readl(reg_base + CQSPI_REG_CONFIG);
	reg |= CQSPI_REG_CONFIG_ENABLE_MASK;
	writel(reg, reg_base + CQSPI_REG_CONFIG);
}

static void cqspi_controller_disable(struct cqspi_st *cqspi)
{
	void __iomem *reg_base = cqspi->iobase;
	unsigned int reg;

	reg = readl(reg_base + CQSPI_REG_CONFIG);
	reg &= ~CQSPI_REG_CONFIG_ENABLE_MASK;
	writel(reg, reg_base + CQSPI_REG_CONFIG);
}

static void cqspi_chipselect(struct cqspi_st *cqspi,
			     unsigned int chip_select,
			     unsigned int decoder_enable)
{
	void __iomem *reg_base = cqspi->iobase;
	unsigned int reg;

	reg = readl(reg_base + CQSPI_REG_CONFIG);
	if (decoder_enable) {
		reg |= CQSPI_REG_CONFIG_DECODE_MASK;
	} else {
		reg &= ~CQSPI_REG_CONFIG_DECODE_MASK;

		/* Convert CS if without decoder.
		 * CS0 to 4b'1110
		 * CS1 to 4b'1101
		 * CS2 to 4b'1011
		 * CS3 to 4b'0111
		 */
		chip_select = 0xF & ~(1 << chip_select);
	}

	reg &= ~(CQSPI_REG_CONFIG_CHIPSELECT_MASK
		 << CQSPI_REG_CONFIG_CHIPSELECT_LSB);
	reg |= (chip_select & CQSPI_REG_CONFIG_CHIPSELECT_MASK)
		<< CQSPI_REG_CONFIG_CHIPSELECT_LSB;
	writel(reg, reg_base + CQSPI_REG_CONFIG);
}

static unsigned int calculate_ticks_for_ns(unsigned int ref_clk_hz,
					   unsigned int ns_val)
{
	unsigned int ticks;

	ticks = ref_clk_hz;
	ticks /= 1000;
	ticks *= ns_val;
	ticks += 999999;
	ticks /= 1000000;
	return ticks;
}

static void cqspi_delay(struct cqspi_st *cqspi,
			unsigned int ref_clk_hz, unsigned int sclk_hz)
{
	void __iomem *reg_base = cqspi->iobase;
	struct cqspi_flash_pdata *f_pdata;
	unsigned int ref_clk_ns;
	unsigned int sclk_ns;
	unsigned int tshsl, tchsh, tslch, tsd2d;
	unsigned int reg;
	unsigned int tsclk;

	if (cqspi->no_reconfig)
		return;

	f_pdata = &cqspi->f_pdata[cqspi->current_cs];

	/* Convert to ns. */
	ref_clk_ns = NSEC_PER_SEC / ref_clk_hz;

	/* Convert to ns. */
	sclk_ns = NSEC_PER_SEC / sclk_hz;

	/* calculate the number of ref ticks for one sclk tick */
	tsclk = (ref_clk_hz + sclk_hz - 1) / sclk_hz;

	tshsl = calculate_ticks_for_ns(ref_clk_hz, f_pdata->tshsl_ns);
	/* this particular value must be at least one sclk */
	if (tshsl < tsclk)
		tshsl = tsclk;

	tchsh = calculate_ticks_for_ns(ref_clk_hz, f_pdata->tchsh_ns);
	tslch = calculate_ticks_for_ns(ref_clk_hz, f_pdata->tslch_ns);
	tsd2d = calculate_ticks_for_ns(ref_clk_hz, f_pdata->tsd2d_ns);

	reg = ((tshsl & CQSPI_REG_DELAY_TSHSL_MASK)
	       << CQSPI_REG_DELAY_TSHSL_LSB);
	reg |= ((tchsh & CQSPI_REG_DELAY_TCHSH_MASK)
		<< CQSPI_REG_DELAY_TCHSH_LSB);
	reg |= ((tslch & CQSPI_REG_DELAY_TSLCH_MASK)
		<< CQSPI_REG_DELAY_TSLCH_LSB);
	reg |= ((tsd2d & CQSPI_REG_DELAY_TSD2D_MASK)
		<< CQSPI_REG_DELAY_TSD2D_LSB);
	writel(reg, reg_base + CQSPI_REG_DELAY);
}

static void cqspi_config_baudrate_div(struct cqspi_st *cqspi,
				      unsigned int ref_clk_hz,
				      unsigned int sclk_hz)
{
	void __iomem *reg_base = cqspi->iobase;
	unsigned int reg;
	unsigned int div;

	reg = readl(reg_base + CQSPI_REG_CONFIG);
	reg &= ~(CQSPI_REG_CONFIG_BAUD_MASK << CQSPI_REG_CONFIG_BAUD_LSB);

	div = ref_clk_hz / sclk_hz;

	/* Recalculate the baudrate divisor based on QSPI specification. */
	if (div > 32)
		div = 32;

	/* Check if even number. */
	if (div & 1)
		div = (div / 2);
	else
		div = (div / 2) - 1;

	div = (div & CQSPI_REG_CONFIG_BAUD_MASK) << CQSPI_REG_CONFIG_BAUD_LSB;
	reg |= div;
	writel(reg, reg_base + CQSPI_REG_CONFIG);
}

static void cqspi_readdata_capture(struct cqspi_st *cqspi,
				   unsigned int bypass, unsigned int delay)
{
	void __iomem *reg_base = cqspi->iobase;
	unsigned int reg;

	if (cqspi->no_reconfig)
		return;

	reg = readl(reg_base + CQSPI_REG_READCAPTURE);

	if (bypass)
		reg |= (1 << CQSPI_REG_READCAPTURE_BYPASS_LSB);
	else
		reg &= ~(1 << CQSPI_REG_READCAPTURE_BYPASS_LSB);

	reg &= ~(CQSPI_REG_READCAPTURE_DELAY_MASK
		 << CQSPI_REG_READCAPTURE_DELAY_LSB);

	reg |= ((delay & CQSPI_REG_READCAPTURE_DELAY_MASK)
		<< CQSPI_REG_READCAPTURE_DELAY_LSB);

	writel(reg, reg_base + CQSPI_REG_READCAPTURE);
}

static void cqspi_switch_cs(struct cqspi_st *cqspi, unsigned int cs)
{
	unsigned int reg;
	struct cqspi_flash_pdata *f_pdata = &cqspi->f_pdata[cs];
	void __iomem *reg_base = cqspi->iobase;
	struct spi_nor *nor = &f_pdata->nor;

	cqspi_controller_disable(cqspi);

	/* configure page size and block size. */
	reg = readl(reg_base + CQSPI_REG_SIZE);
	reg &= ~(CQSPI_REG_SIZE_PAGE_MASK << CQSPI_REG_SIZE_PAGE_LSB);
	reg &= ~(CQSPI_REG_SIZE_BLOCK_MASK << CQSPI_REG_SIZE_BLOCK_LSB);
	reg |= (nor->page_size << CQSPI_REG_SIZE_PAGE_LSB);
	reg |= (f_pdata->block_size << CQSPI_REG_SIZE_BLOCK_LSB);
	reg &= ~CQSPI_REG_SIZE_ADDRESS_MASK;
	reg |= (nor->addr_width - 1);
	writel(reg, reg_base + CQSPI_REG_SIZE);

	/* configure the chip select */
	cqspi_chipselect(cqspi, cs, cqspi->is_decoded_cs);

	cqspi_controller_enable(cqspi);
}

static int cqspi_configure(struct spi_nor *nor)
{
	struct cqspi_st *cqspi = nor->priv;
	int cs = cqspi_find_chipselect(nor);
	struct cqspi_flash_pdata *f_pdata;
	unsigned int sclk;

	/* Switch chip select. */
	if (cqspi->current_cs != cs) {
		cqspi->current_cs = cs;
		cqspi_switch_cs(cqspi, cs);
	}

	/* Setup baudrate divisor and delays */
	f_pdata = &cqspi->f_pdata[cqspi->current_cs];
	sclk = f_pdata->clk_rate;
	if (cqspi->sclk != sclk) {
		cqspi->sclk = sclk;
		cqspi_controller_disable(cqspi);
		cqspi_config_baudrate_div(cqspi,
					  cqspi->master_ref_clk_hz, sclk);
		cqspi_delay(cqspi, cqspi->master_ref_clk_hz, sclk);
		cqspi_readdata_capture(cqspi, 1, f_pdata->read_delay);
		cqspi_controller_enable(cqspi);
	}
	return 0;
}

static int cqspi_set_protocol(struct spi_nor *nor, const int read)
{
	struct cqspi_st *cqspi = nor->priv;
	struct cqspi_flash_pdata *f_pdata;
	int cs = cqspi_find_chipselect(nor);

	if (cs < 0)
		return -EINVAL;

	f_pdata = &cqspi->f_pdata[cs];

	f_pdata->inst_width = CQSPI_INST_TYPE_SINGLE;
	f_pdata->addr_width = CQSPI_INST_TYPE_SINGLE;
	f_pdata->data_width = CQSPI_INST_TYPE_SINGLE;

	if (read) {
		switch (nor->read_proto) {
		case SNOR_PROTO_1_1_1:
			f_pdata->data_width = CQSPI_INST_TYPE_SINGLE;
			break;
		case SNOR_PROTO_1_1_2:
			f_pdata->data_width = CQSPI_INST_TYPE_DUAL;
			break;
		case SNOR_PROTO_1_1_4:
			f_pdata->data_width = CQSPI_INST_TYPE_QUAD;
			break;
		default:
			return -EINVAL;
		}
	}

	cqspi_configure(nor);

	return 0;
}

static void cqspi_write(struct spi_nor *nor, loff_t to,
			size_t len, size_t *retlen, const u_char *buf)
{
	int ret;

	if (!IS_ENABLED(CONFIG_MTD_WRITE))
		return;

	ret = cqspi_set_protocol(nor, 0);
	if (ret)
		return;

	ret = cqspi_indirect_write_setup(nor, to);
	if (ret == 0) {
		ret = cqspi_indirect_write_execute(nor, buf, len);
		if (ret == 0)
			*retlen += len;
	}
}

static int cqspi_read(struct spi_nor *nor, loff_t from,
		      size_t len, size_t *retlen, u_char *buf)
{
	int ret;

	ret = cqspi_set_protocol(nor, 1);
	if (ret)
		return ret;

	ret = cqspi_indirect_read_setup(nor, from);
	if (ret == 0) {
		ret = cqspi_indirect_read_execute(nor, buf, len);
		if (ret == 0)
			*retlen += len;
	}
	return ret;
}

static int cqspi_erase(struct spi_nor *nor, loff_t offs)
{
	int ret;

	ret = cqspi_set_protocol(nor, 0);
	if (ret)
		return ret;

	/* Send write enable, then erase commands. */
	ret = nor->write_reg(nor, SPINOR_OP_WREN, NULL, 0);
	if (ret)
		return ret;

	/* Set up command buffer. */
	ret = cqspi_command_write_addr(nor, nor->erase_opcode, offs);
	if (ret)
		return ret;

	return 0;
}

static int cqspi_read_reg(struct spi_nor *nor, u8 opcode, u8 *buf, int len)
{
	int ret;

	ret = cqspi_set_protocol(nor, 0);
	if (!ret)
		ret = cqspi_command_read(nor, &opcode, 1, buf, len);

	return ret;
}

static int cqspi_write_reg(struct spi_nor *nor, u8 opcode, u8 *buf, int len)
{
	int ret = 0;

	if (!IS_ENABLED(CONFIG_MTD_WRITE))
		return -ENOTSUPP;

	ret = cqspi_set_protocol(nor, 0);
	if (!ret)
		ret = cqspi_command_write(nor, opcode, buf, len);

	return ret;
}

static int cqspi_of_get_flash_pdata(struct device *dev,
				    struct cqspi_flash_pdata *f_pdata,
				    struct device_node *np)
{
	struct cqspi_st *cqspi = dev->priv;
	void __iomem *reg_base = cqspi->iobase;

	if (!np) {
		f_pdata->block_size = (readl(reg_base + CQSPI_REG_SIZE) >>
					CQSPI_REG_SIZE_BLOCK_LSB) &
					CQSPI_REG_SIZE_BLOCK_MASK;

		cqspi->no_reconfig = true;

		return 0;
	}

	if (of_property_read_u32(np, "cdns,block-size", &f_pdata->block_size)) {
		dev_err(dev, "couldn't determine block-size\n");
		return -ENXIO;
	}

	if (of_property_read_u32(np, "cdns,read-delay", &f_pdata->read_delay)) {
		dev_err(dev, "couldn't determine read-delay\n");
		return -ENXIO;
	}

	if (of_property_read_u32(np, "cdns,tshsl-ns", &f_pdata->tshsl_ns)) {
		dev_err(dev, "couldn't determine tshsl-ns\n");
		return -ENXIO;
	}

	if (of_property_read_u32(np, "cdns,tsd2d-ns", &f_pdata->tsd2d_ns)) {
		dev_err(dev, "couldn't determine tsd2d-ns\n");
		return -ENXIO;
	}

	if (of_property_read_u32(np, "cdns,tchsh-ns", &f_pdata->tchsh_ns)) {
		dev_err(dev, "couldn't determine tchsh-ns\n");
		return -ENXIO;
	}

	if (of_property_read_u32(np, "cdns,tslch-ns", &f_pdata->tslch_ns)) {
		dev_err(dev, "couldn't determine tslch-ns\n");
		return -ENXIO;
	}

	if (of_property_read_u32(np, "spi-max-frequency", &f_pdata->clk_rate)) {
		dev_err(dev, "couldn't determine spi-max-frequency\n");
		return -ENXIO;
	}

	return 0;
}

static int cqspi_parse_dt(struct cqspi_st *cqspi)
{
	struct device_node *np = cqspi->dev->of_node;
	struct device *dev = cqspi->dev;

	cqspi->is_decoded_cs = of_property_read_bool(np, "cdns,is-decoded-cs");

	if (of_property_read_u32(np, "cdns,fifo-depth", &cqspi->fifo_depth)) {
		dev_err(dev, "couldn't determine fifo-depth\n");
		return -ENXIO;
	}

	return 0;
}

static int cqspi_setup_flash(struct device *dev,
			     struct cqspi_flash_pdata *f_pdata,
			     struct device_node *np)
{
	const struct spi_nor_hwcaps hwcaps = {
		.mask = SNOR_HWCAPS_READ |
			SNOR_HWCAPS_READ_FAST |
			SNOR_HWCAPS_READ_1_1_2 |
			SNOR_HWCAPS_READ_1_1_4 |
			SNOR_HWCAPS_PP,
	};
	struct cqspi_st *cqspi = dev->priv;
	struct mtd_info *mtd;
	struct spi_nor *nor;
	int ret;

	ret = cqspi_of_get_flash_pdata(dev, f_pdata, np);
	if (ret)
		goto probe_failed;

	nor = &f_pdata->nor;
	mtd = &f_pdata->mtd;

	nor->mtd = mtd;

	if (np) {
		nor->dev = xzalloc(sizeof(*nor->dev));

		dev_set_name(nor->dev, "%s", np->name);

		nor->dev->of_node = np;
		nor->dev->id = DEVICE_ID_SINGLE;
		nor->dev->parent = dev;
		ret = register_device(nor->dev);

		if (ret)
			return ret;

		mtd->dev.parent = nor->dev;
	} else {
		nor->dev = dev;
	}

	nor->priv = cqspi;
	mtd->priv = nor;

	nor->read_reg = cqspi_read_reg;
	nor->write_reg = cqspi_write_reg;
	nor->read = cqspi_read;
	nor->write = cqspi_write;
	nor->erase = cqspi_erase;

	ret = spi_nor_scan(nor, NULL, &hwcaps, false);
	if (ret)
		goto probe_failed;

	ret = add_mtd_device(mtd, NULL, DEVICE_ID_DYNAMIC);
	if (ret)
		goto probe_failed;

	return 0;

probe_failed:
	dev_err(dev, "probing for flashchip failed\n");
	return ret;

}

static void cqspi_controller_init(struct cqspi_st *cqspi)
{
	cqspi_controller_disable(cqspi);

	/* Configure the remap address register, no remap */
	writel(0, cqspi->iobase + CQSPI_REG_REMAP);

	/* Disable all interrupts */
	writel(0, cqspi->iobase + CQSPI_REG_IRQMASK);

	cqspi_controller_enable(cqspi);
}

static int cqspi_probe(struct device *dev)
{
	struct resource *iores;
	struct device_node *np = dev->of_node;
	struct cqspi_st *cqspi;
	struct cadence_qspi_platform_data *pdata = dev->platform_data;
	int ret;

	cqspi = kzalloc(sizeof(*cqspi), GFP_KERNEL);
	if (!cqspi)
		return -ENOMEM;

	cqspi->dev = dev;
	dev->priv = cqspi;

	if (pdata) {
		cqspi->is_decoded_cs = pdata->is_decoded_cs;
		cqspi->fifo_depth = pdata->fifo_depth;
	} else {
		ret = cqspi_parse_dt(cqspi);
		if (ret) {
			dev_err(dev, "Get platform data failed.\n");
			return -ENODEV;
		}
	}

	cqspi->qspi_clk = clk_get(dev, NULL);
	if (IS_ERR(cqspi->qspi_clk)) {
		dev_err(dev, "cannot get qspi clk\n");
		ret = PTR_ERR(cqspi->qspi_clk);
		goto probe_failed;
	}
	cqspi->master_ref_clk_hz = clk_get_rate(cqspi->qspi_clk);

	clk_enable(cqspi->qspi_clk);

	iores = dev_request_mem_resource(dev, 0);
	if (IS_ERR(iores))
		return PTR_ERR(iores);
	cqspi->iobase = IOMEM(iores->start);

	iores = dev_request_mem_resource(dev, 1);
	if (IS_ERR(iores))
		return PTR_ERR(iores);
	cqspi->ahb_base = IOMEM(iores->start);

	cqspi_wait_idle(cqspi);
	cqspi_controller_init(cqspi);
	cqspi->current_cs = -1;
	cqspi->sclk = 0;

	if (!dev->of_node) {
		struct cqspi_flash_pdata *f_pdata;

		f_pdata = &cqspi->f_pdata[0];

		ret = cqspi_setup_flash(dev, f_pdata, NULL);
		if (ret)
			goto probe_failed;
	} else {
		/* Get flash device data */
		for_each_available_child_of_node(dev->of_node, np) {
			struct cqspi_flash_pdata *f_pdata;
			unsigned int cs;
			if (of_property_read_u32(np, "reg", &cs)) {
				dev_err(dev, "couldn't determine chip select\n");
				return -ENXIO;
			}
			if (cs > CQSPI_MAX_CHIPSELECT) {
				dev_err(dev, "chip select %d out of range\n", cs);
				return -ENXIO;
			}
			f_pdata = &cqspi->f_pdata[cs];

			ret = cqspi_setup_flash(dev, f_pdata, np);
			if (ret)
				goto probe_failed;
		}
	}

	dev_info(dev, "Cadence QSPI NOR flash driver\n");
	return 0;

probe_failed:
	dev_err(dev, "Cadence QSPI NOR probe failed\n");
	return ret;
}

static __maybe_unused struct of_device_id cqspi_dt_ids[] = {
	{.compatible = "cdns,qspi-nor",},
	{ /* end of table */ }
};
MODULE_DEVICE_TABLE(of, cqspi_dt_ids);

static struct driver cqspi_driver = {
	.name = "cadence_qspi",
	.probe = cqspi_probe,
	.of_compatible = DRV_OF_COMPAT(cqspi_dt_ids),
};
device_platform_driver(cqspi_driver);
