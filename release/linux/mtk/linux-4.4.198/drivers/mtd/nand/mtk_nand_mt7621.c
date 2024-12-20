/*
 * SPDX-License-Identifier: GPL-2.0
 * Driver for MediaTek SLC NAND Flash interface controller
 *
 * Copyright (C) 2018 MediaTek Inc.
 * Authors:	Xiangsheng Hou	<xiangsheng.hou@mediatek.com>
 *		Weijie Gao	<weijie.gao@mediatek.com>
 *
 */

#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/mtd/nand.h>
#include <linux/mtd/mtd.h>
#include <linux/module.h>
#include <linux/iopoll.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/pm_runtime.h>
#include <linux/mtd/partitions.h>
#include <linux/sizes.h>
#include "mtk_nand_mt7621.h"

#define MTK_NAME		"mtk-nand"

static int mtk_nfc_page_erase_write(struct mtd_info *mtd,
	struct nand_chip *chip, const uint8_t *buf, const uint8_t *oob,
	int page);

static int mtk_nfc_pre_jffs2_fixup(struct mtd_info *mtd,
				   struct nand_chip *chip, int page);

static inline struct mtk_nfc_nand_chip *to_mtk_nand(struct nand_chip *nand)
{
	return container_of(nand, struct mtk_nfc_nand_chip, nand);
}

static inline u8 *data_ptr(struct nand_chip *chip, const u8 *p, int i)
{
	return (u8 *)p + i * chip->ecc.size;
}

static inline u8 *oob_buf_ptr(struct nand_chip *chip, u8 *p, int i)
{
	struct mtk_nfc *nfc = nand_get_controller_data(chip);
	u8 *poi;

	poi = p + i * nfc->caps->fdm_size;

	return poi;
}

static inline u8 *oob_ptr(struct nand_chip *chip, int i)
{
	return oob_buf_ptr(chip, chip->oob_poi, i);
}

static inline u8 *ecc_ptr(struct nand_chip *chip, int i)
{
	struct mtk_nfc *nfc = nand_get_controller_data(chip);
	struct mtk_nfc_nand_chip *mtk_nand = to_mtk_nand(chip);
	u8 *poi;

	poi = chip->oob_poi + chip->ecc.steps * nfc->caps->fdm_size
		+ i * (mtk_nand->spare_per_sector - nfc->caps->fdm_size);

	return poi;
}

static inline int mtk_data_len(struct nand_chip *chip)
{
	struct mtk_nfc_nand_chip *mtk_nand = to_mtk_nand(chip);

	return chip->ecc.size + mtk_nand->spare_per_sector;
}

static inline u8 *mtk_data_ptr(struct nand_chip *chip,  int i)
{
	struct mtk_nfc *nfc = nand_get_controller_data(chip);

	return nfc->buffer + i * mtk_data_len(chip);
}

static inline u8 *mtk_oob_ptr(struct nand_chip *chip, int i)
{
	struct mtk_nfc *nfc = nand_get_controller_data(chip);

	return nfc->buffer + i * mtk_data_len(chip) + chip->ecc.size;
}

static inline u8 *mtk_ecc_ptr(struct nand_chip *chip, int i)
{
	struct mtk_nfc *nfc = nand_get_controller_data(chip);

	return mtk_oob_ptr(chip, i) + nfc->caps->fdm_size;
}

static inline void nfi_clear_reg16(struct mtk_nfc *nfc, u32 val, u32 reg)
{
	u16 temp_val = 0;

	temp_val = readw_relaxed(nfc->regs + reg);
	temp_val &= ~val;
	writew(temp_val, nfc->regs + reg);
}

static inline void nfi_set_reg16(struct mtk_nfc *nfc, u32 val, u32 reg)
{
	u16 temp_val = 0;

	temp_val = readw_relaxed(nfc->regs + reg);
	temp_val |= val;
	writew(temp_val, nfc->regs + reg);
}

static inline void nfi_writel(struct mtk_nfc *nfc, u32 val, u32 reg)
{
	writel(val, nfc->regs + reg);
}

static inline void nfi_writew(struct mtk_nfc *nfc, u16 val, u32 reg)
{
	writew(val, nfc->regs + reg);
}

static inline u32 nfi_readl(struct mtk_nfc *nfc, u32 reg)
{
	return readl_relaxed(nfc->regs + reg);
}

static inline u16 nfi_readw(struct mtk_nfc *nfc, u32 reg)
{
	return readw_relaxed(nfc->regs + reg);
}

static void mtk_nfc_hw_reset(struct mtk_nfc *nfc)
{
	struct device *dev = nfc->dev;
	u32 val;
	int ret;

	/* reset all registers and force the NFI master to terminate */
	nfi_writel(nfc, CON_FIFO_FLUSH | CON_NFI_RST, NFI_CON);

	/* wait for the master to finish the last transaction */
	ret = readl_poll_timeout(nfc->regs + NFI_MASTER_STA, val,
				 !(val & MASTER_STA_MASK), 50,
				 MTK_RESET_TIMEOUT);
	if (ret)
		dev_warn(dev, "master active in reset [0x%x] = 0x%x\n",
			 NFI_MASTER_STA, val);

	/* ensure any status register affected by the NFI master is reset */
	nfi_writel(nfc, CON_FIFO_FLUSH | CON_NFI_RST, NFI_CON);
	nfi_writew(nfc, STAR_DE, NFI_STRDATA);
}

static int mtk_nfc_send_command(struct mtk_nfc *nfc, u8 command)
{
	struct device *dev = nfc->dev;
	u32 val;
	int ret;

	nfi_writel(nfc, command, NFI_CMD);

	ret = readl_poll_timeout_atomic(nfc->regs + NFI_STA, val,
					!(val & STA_CMD), 10,  MTK_TIMEOUT);
	if (ret) {
		dev_warn(dev, "nfi core timed out entering command mode\n");
		return -EIO;
	}

	return 0;
}

static int mtk_nfc_send_address(struct mtk_nfc *nfc, int addr)
{
	struct device *dev = nfc->dev;
	u32 val;
	int ret;

	nfi_writel(nfc, addr, NFI_COLADDR);
	nfi_writel(nfc, 0, NFI_ROWADDR);
	nfi_writew(nfc, 1, NFI_ADDRNOB);

	ret = readl_poll_timeout_atomic(nfc->regs + NFI_STA, val,
					!(val & STA_ADDR), 10, MTK_TIMEOUT);
	if (ret) {
		dev_warn(dev, "nfi core timed out entering address mode\n");
		return -EIO;
	}

	return 0;
}

static int mtk_nfc_hw_runtime_config(struct mtd_info *mtd)
{
	struct nand_chip *chip = mtd_to_nand(mtd);
	struct mtk_nfc_nand_chip *mtk_nand = to_mtk_nand(chip);
	struct mtk_nfc *nfc = nand_get_controller_data(chip);
	struct device *dev = nfc->dev;
	u32 fmt, spare_bit, ecc_bits;

	if (!mtd->writesize)
		return 0;

	chip->ecc.size = nfc->caps->sector_size;
	chip->ecc.steps = mtd->writesize / chip->ecc.size;
	mtk_nand->spare_per_sector = mtd->oobsize / chip->ecc.steps;

	if (mtk_nand->spare_per_sector >= 28) {
		spare_bit = PAGEFMT_SPARE_28;
		chip->ecc.strength = 12;
		mtk_nand->spare_per_sector = 28;
	} else if (mtk_nand->spare_per_sector >= 27) {
		spare_bit = PAGEFMT_SPARE_27;
		chip->ecc.strength = 8;
		mtk_nand->spare_per_sector = 27;
	} else if (mtk_nand->spare_per_sector >= 26) {
		spare_bit = PAGEFMT_SPARE_26;
		chip->ecc.strength = 8;
		mtk_nand->spare_per_sector = 26;
	} else if (mtk_nand->spare_per_sector >= 16) {
		spare_bit = PAGEFMT_SPARE_16;
		chip->ecc.strength = 4;
		mtk_nand->spare_per_sector = 16;
	} else {
		dev_err(dev, "MTK NFI not support oobsize: %x\n",
			mtk_nand->spare_per_sector);
		return -EINVAL;
	}

	mtk_nand->oobsize_avail = chip->ecc.steps * mtk_nand->spare_per_sector;
	mtk_nand->trailing_bytes = mtd->oobsize - mtk_nand->oobsize_avail;

	switch (mtd->writesize) {
	case 512:
		fmt = PAGEFMT_512;
		break;
	case SZ_2K:
		fmt = PAGEFMT_2K;
		break;
	case SZ_4K:
		fmt = PAGEFMT_4K;
		break;
	default:
		dev_err(nfc->dev, "invalid page len: %d\n", mtd->writesize);
		return -EINVAL;
	}

	fmt |= spare_bit << nfc->caps->pageformat_spare_shift;
	fmt |= nfc->caps->fdm_size << PAGEFMT_FDM_SHIFT;
	fmt |= nfc->caps->fdm_ecc_size << PAGEFMT_FDM_ECC_SHIFT;
	nfi_writel(nfc, fmt, NFI_PAGEFMT);

	nfc->ecc_cfg.strength = chip->ecc.strength;
	nfc->ecc_cfg.len = chip->ecc.size + nfc->caps->fdm_ecc_size;

	ecc_bits = chip->ecc.strength * nfc->ecc->caps->parity_bits;
	chip->ecc.bytes = DIV_ROUND_UP(ecc_bits, 8);

	mtk_nand->ecc_spare_bytes = mtk_nand->spare_per_sector -
				  nfc->caps->fdm_ecc_size -
				  chip->ecc.bytes;

	nfi_writel(nfc, ACCESS_TIMING_DFL, NFI_ACCCON);

	return 0;
}

static void mtk_nfc_select_chip(struct mtd_info *mtd, int chip)
{
	struct nand_chip *nand = mtd_to_nand(mtd);
	struct mtk_nfc *nfc = nand_get_controller_data(nand);
	struct mtk_nfc_nand_chip *mtk_nand = to_mtk_nand(nand);

	if (chip < 0)
		return;

	mtk_nfc_hw_runtime_config(mtd);

	nfi_writel(nfc, mtk_nand->sels[chip], NFI_CSEL);
}

static int mtk_nfc_dev_ready(struct mtd_info *mtd)
{
	struct mtk_nfc *nfc = nand_get_controller_data(mtd_to_nand(mtd));

	if (nfi_readl(nfc, NFI_STA) & STA_BUSY)
		return 0;

	return 1;
}

static void mtk_nfc_cmd_ctrl(struct mtd_info *mtd, int dat, unsigned int ctrl)
{
	struct mtk_nfc *nfc = nand_get_controller_data(mtd_to_nand(mtd));

	if (ctrl & NAND_ALE) {
		mtk_nfc_send_address(nfc, dat);
	} else if (ctrl & NAND_CLE) {
		mtk_nfc_hw_reset(nfc);

		nfi_writew(nfc, CNFG_OP_CUST, NFI_CNFG);
		mtk_nfc_send_command(nfc, dat);
	}
}

static inline void mtk_nfc_wait_ioready(struct mtk_nfc *nfc)
{
	int rc;
	u8 val;

	rc = readb_poll_timeout_atomic(nfc->regs + NFI_PIO_DIRDY, val,
				       val & PIO_DI_RDY, 10, MTK_TIMEOUT);
	if (rc < 0)
		dev_err(nfc->dev, "data not ready\n");
}

static u32 mtk_nfc_pio_read(struct mtd_info *mtd, int byterw)
{
	struct nand_chip *chip = mtd_to_nand(mtd);
	struct mtk_nfc *nfc = nand_get_controller_data(chip);
	u32 reg;

	/* after each byte read, the NFI_STA reg is reset by the hardware */
	reg = nfi_readl(nfc, NFI_STA) & NFI_FSM_MASK;
	if (reg != NFI_FSM_CUSTDATA) {
		if (byterw)
			nfi_set_reg16(nfc, CNFG_BYTE_RW, NFI_CNFG);
		else
			nfi_clear_reg16(nfc, CNFG_BYTE_RW, NFI_CNFG);

		reg = nfi_readw(nfc, NFI_CNFG);
		reg |= CNFG_READ_EN;
		nfi_writew(nfc, reg, NFI_CNFG);

		/*
		 * set to max sector to allow the HW to continue reading over
		 * unaligned accesses
		 */
		reg = (nfc->caps->max_sector << CON_SEC_SHIFT) | CON_BRD;
		nfi_writel(nfc, reg, NFI_CON);

		/* trigger to fetch data */
		nfi_writew(nfc, STAR_EN, NFI_STRDATA);
	}

	mtk_nfc_wait_ioready(nfc);

	return nfi_readl(nfc, NFI_DATAR);
}

static inline u8 mtk_nfc_read_byte(struct mtd_info *mtd)
{
	return mtk_nfc_pio_read(mtd, 1) & 0xff;
}

static void mtk_nfc_read_buf(struct mtd_info *mtd, u8 *buf, int len)
{
	int i;
	u32 *p = (u32 *) buf;

	if ((u32) buf % sizeof(u32) || len % sizeof(u32)) {
		for (i = 0; i < len; i++)
			buf[i] = mtk_nfc_pio_read(mtd, 1);
	} else {
		for (i = 0; i < (len / sizeof(u32)); i++)
			p[i] = mtk_nfc_pio_read(mtd, 0);
	}
}

static void mtk_nfc_pio_write(struct mtd_info *mtd, u32 val, int byterw)
{
	struct mtk_nfc *nfc = nand_get_controller_data(mtd_to_nand(mtd));
	u32 reg;

	reg = nfi_readl(nfc, NFI_STA) & NFI_FSM_MASK;

	if (reg != NFI_FSM_CUSTDATA) {
		if (byterw)
			nfi_set_reg16(nfc, CNFG_BYTE_RW, NFI_CNFG);
		else
			nfi_clear_reg16(nfc, CNFG_BYTE_RW, NFI_CNFG);

		reg = nfc->caps->max_sector << CON_SEC_SHIFT | CON_BWR;
		nfi_writew(nfc, reg, NFI_CON);

		nfi_writew(nfc, STAR_EN, NFI_STRDATA);
	}

	mtk_nfc_wait_ioready(nfc);
	nfi_writel(nfc, val, NFI_DATAW);
}


static void mtk_nfc_write_byte(struct mtd_info *mtd, u8 byte)
{
	mtk_nfc_pio_write(mtd, byte, 1);
}

static void mtk_nfc_write_buf(struct mtd_info *mtd, const u8 *buf, int len)
{
	int i;
	const u32 *p = (const u32 *) buf;

	if ((u32) buf % sizeof(u32) || len % sizeof(u32)) {
		for (i = 0; i < len; i++)
			mtk_nfc_pio_write(mtd, buf[i], 1);
	} else {
		for (i = 0; i < (len / sizeof(u32)); i++)
			mtk_nfc_pio_write(mtd, p[i], 0);
	}
}

static inline void mtk_nfc_read_fdm(struct nand_chip *chip, u32 start,
				    u32 sectors)
{
	struct mtk_nfc *nfc = nand_get_controller_data(chip);
	u32 vall, valm;
	u8 *oobptr;
	int i, j;

	for (i = 0; i < sectors; i++) {
		oobptr = oob_ptr(chip, start + i);
		vall = nfi_readl(nfc, NFI_FDML(start + i));
		valm = nfi_readl(nfc, NFI_FDMM(start + i));

		for (j = 0; j < nfc->caps->fdm_size; j++)
			oobptr[j] = (j >= 4 ? valm : vall) >> ((j % 4) * 8);
	}
}

static inline void mtk_nfc_write_fdm(struct nand_chip *chip)
{
	struct mtk_nfc *nfc = nand_get_controller_data(chip);
	u32 vall, valm;
	u8 *oobptr;
	int i, j;

	for (i = 0; i < chip->ecc.steps; i++) {
		oobptr = oob_ptr(chip, i);
		vall = 0;
		valm = 0;
		for (j = 0; j < 8; j++) {
			if (j < 4)
				vall |= (j < nfc->caps->fdm_size ? oobptr[j]
					: 0xff) << (j * 8);
			else
				valm |= (j < nfc->caps->fdm_size ? oobptr[j]
					: 0xff) << ((j - 4) * 8);
		}
		nfi_writel(nfc, vall, NFI_FDML(i));
		nfi_writel(nfc, valm, NFI_FDMM(i));
	}
}

static int mtk_nfc_oneshot_write_buf(struct mtd_info *mtd,
				     struct nand_chip *chip, const u8 *buf,
				     u32 len)
{
	struct mtk_nfc *nfc = nand_get_controller_data(chip);
	u32 val;
	int ret;

	nfi_clear_reg16(nfc, CNFG_READ_EN | CNFG_AUTO_FMT_EN
		| CNFG_HW_ECC_EN, NFI_CNFG);

	mtk_nfc_write_buf(mtd, buf, len);

	ret = readw_poll_timeout_atomic(nfc->regs + NFI_FIFOSTA, val,
					val & WR_EMPTY, 10, MTK_TIMEOUT);

	nfi_writew(nfc, 0, NFI_CON);

	return ret;
}

static int mtk_nfc_write_page_ecc_trailings(struct mtd_info *mtd,
					    struct nand_chip *chip, bool ecc)
{
	struct mtk_nfc *nfc = nand_get_controller_data(chip);
	struct mtk_nfc_nand_chip *mtk_nand = to_mtk_nand(chip);
	int i, ret;
	u32 offs;

	if (!ecc || !mtk_nand->ecc_spare_bytes)
		goto trailing_only;

	for (i = 0; i < chip->ecc.steps; i++) {
		offs = (uintptr_t)mtk_ecc_ptr(chip, i) -
		       (uintptr_t)nfc->buffer + chip->ecc.bytes;

		chip->cmdfunc(mtd, NAND_CMD_RNDIN, offs, -1);

		ret = mtk_nfc_oneshot_write_buf(mtd, chip,
			ecc_ptr(chip, i) + chip->ecc.bytes,
			mtk_nand->ecc_spare_bytes);

		if (ret)
			return ret;
	}

trailing_only:
	/* Write trailing bytes */
	if (mtk_nand->trailing_bytes) {
		/* Offset of the trailing bytes can't be accessed by NFI */
		offs = mtd->oobsize - mtk_nand->trailing_bytes;

		chip->cmdfunc(mtd, NAND_CMD_RNDIN, mtd->writesize + offs, -1);

		ret = mtk_nfc_oneshot_write_buf(mtd, chip, chip->oob_poi + offs,
						mtk_nand->trailing_bytes);

		if (ret)
			return ret;
	}

	return 0;
}

/* Check if the whole page is empty */
static int mtk_nfc_check_empty_page_full(struct mtd_info *mtd,
					 struct nand_chip *chip, const u8 *buf)
{
	u32 i;

	if (buf) {
		for (i = 0; i < mtd->writesize; i++)
			if (buf[i] != 0xff)
				return 0;
	}

	for (i = 0; i < mtd->oobsize; i++)
		if (chip->oob_poi[i] != 0xff)
			return 0;

	return 1;
}

/* Check if the raw oob parts are empty */
static int mtk_nfc_check_empty_page_spare_trailing(struct mtd_info *mtd,
						   struct nand_chip *chip)
{
	struct mtk_nfc_nand_chip *mtk_nand = to_mtk_nand(chip);
	const u8 *ptr;
	u32 i, j;

	if (mtk_nand->ecc_spare_bytes) {
		for (i = 0; i < chip->ecc.steps; i++) {
			ptr = ecc_ptr(chip, i) + chip->ecc.bytes;
			for (j = 0; j < mtk_nand->ecc_spare_bytes; j++)
				if (ptr[j] != 0xff)
					return 0;
		}
	}

	if (mtk_nand->trailing_bytes) {
		ptr = chip->oob_poi + mtd->oobsize - mtk_nand->trailing_bytes;
		for (i = 0; i < mtk_nand->trailing_bytes; i++)
			if (ptr[i] != 0xff)
				return 0;
	}

	return 1;
}

/* Check if the ecc-protected parts are empty */
static int mtk_nfc_check_empty_page(struct mtd_info *mtd,
				    struct nand_chip *chip, const u8 *buf)
{
	struct mtk_nfc *nfc = nand_get_controller_data(chip);
	u32 i, j;
	u8 *oob_poi;

	if (buf) {
		for (i = 0; i < mtd->writesize; i++)
			if (buf[i] != 0xff)
				return 0;
	}

	for (i = 0; i < chip->ecc.steps; i++) {
		oob_poi = oob_ptr(chip, i);
		for (j = 0; j < nfc->caps->fdm_ecc_size; j++)
			if (oob_poi[j] != 0xff)
				return 0;
	}

	return 1;
}

static int _mtk_nfc_write_page_raw(struct mtd_info *mtd, struct nand_chip *chip,
				  const u8 *buf, int oob_on, int page,
				  int write_ecc_protected)
{
	struct mtk_nfc *nfc = nand_get_controller_data(chip);
	struct mtk_nfc_nand_chip *mtk_nand = to_mtk_nand(chip);
	struct device *dev = nfc->dev;
	u32 i, ret, reg;

	memset(nfc->buffer, 0xff, mtd->writesize + mtd->oobsize);

	for (i = 0; i < chip->ecc.steps; i++) {
		if (write_ecc_protected) {
			memcpy(mtk_oob_ptr(chip, i), oob_ptr(chip, i),
			       nfc->caps->fdm_size);

			memcpy(mtk_ecc_ptr(chip, i), ecc_ptr(chip, i),
			       chip->ecc.bytes);
		}

		if (mtk_nand->ecc_spare_bytes) {
			memcpy(mtk_ecc_ptr(chip, i) + chip->ecc.bytes,
			       ecc_ptr(chip, i) + chip->ecc.bytes,
			       mtk_nand->ecc_spare_bytes);
		}
	}

	if (buf && write_ecc_protected) {
		for (i = 0; i < chip->ecc.steps; i++)
			memcpy(mtk_data_ptr(chip, i), data_ptr(chip, buf, i),
			       chip->ecc.size);
	}

	nfi_clear_reg16(nfc, CNFG_READ_EN | CNFG_AUTO_FMT_EN
		| CNFG_HW_ECC_EN, NFI_CNFG);
	mtk_nfc_write_buf(mtd, nfc->buffer,
			  mtd->writesize + mtk_nand->oobsize_avail);

	ret = readl_poll_timeout_atomic(nfc->regs + NFI_ADDRCNTR, reg,
					ADDRCNTR_SEC(reg) >= chip->ecc.steps,
					10, MTK_TIMEOUT);
	if (ret)
		dev_err(dev, "raw write timeout\n");

	nfi_writel(nfc, 0, NFI_CON);

	if (ret)
		return ret;

	return mtk_nfc_write_page_ecc_trailings(mtd, chip, false);
}

static int mtk_nfc_write_page_raw(struct mtd_info *mtd, struct nand_chip *chip,
				  const u8 *buf, int oob_on, int page)
{
	return _mtk_nfc_write_page_raw(mtd, chip, buf, oob_on, page, 1);
}

static int mtk_nfc_write_page_hwecc(struct mtd_info *mtd,
	struct nand_chip *chip, const u8 *buf, int oob_on, int page)
{
	struct mtk_nfc *nfc = nand_get_controller_data(chip);
	struct device *dev = nfc->dev;
	int ret;
	u32 reg;

	if (mtk_nfc_check_empty_page(mtd, chip, buf)) {
		/*
		 * When the entire page is 0xff including oob data,
		 * do not use ecc engine which will write ecc parity code
		 * back to oob region.
		 *
		 * For 4-bit ecc strength, the ecc parity code of a full
		 * 0xff subpage is 26 20 98 1b 87 6e fc
		 *
		 * Use raw mode instead.
		 */
		return _mtk_nfc_write_page_raw(mtd, chip, NULL, oob_on, page,
					       0);
	}

	nfi_clear_reg16(nfc, CNFG_READ_EN, NFI_CNFG);
	nfi_set_reg16(nfc, CNFG_AUTO_FMT_EN | CNFG_HW_ECC_EN, NFI_CNFG);

	nfc->ecc_cfg.op = ECC_ENCODE;
	mtk_ecc_init(nfc, nfc->ecc, &nfc->ecc_cfg);
	mtk_ecc_enable(nfc->ecc, &nfc->ecc_cfg);

	mtk_nfc_write_fdm(chip);
	reg = chip->ecc.steps << CON_SEC_SHIFT | CON_BWR;
	nfi_writew(nfc, reg, NFI_CON);
	mtk_nfc_write_buf(mtd, buf, mtd->writesize);

	ret = readl_poll_timeout_atomic(nfc->regs + NFI_ADDRCNTR, reg,
					ADDRCNTR_SEC(reg) >= chip->ecc.steps,
					10, MTK_TIMEOUT);
	if (ret)
		dev_err(dev, "hwecc write timeout\n");

	mtk_ecc_disable(nfc->ecc);
	nfi_writel(nfc, 0, NFI_CON);

	if (ret)
		return ret;

	return mtk_nfc_write_page_ecc_trailings(mtd, chip, false);
}

static int mtk_nfc_write_oob_raw(struct mtd_info *mtd, struct nand_chip *chip,
				 int page)
{
	int ret;

	/* Do not write full empty page */
	if (mtk_nfc_check_empty_page_full(mtd, chip, NULL))
		return 0;

	chip->cmdfunc(mtd, NAND_CMD_SEQIN, 0x00, page);

	ret = mtk_nfc_write_page_raw(mtd, chip, NULL, 1, page);
	if (ret < 0)
		return -EIO;

	chip->cmdfunc(mtd, NAND_CMD_PAGEPROG, -1, -1);
	ret = chip->waitfunc(mtd, chip);

	return ret & NAND_STATUS_FAIL ? -EIO : 0;
}

static int mtk_nfc_write_oob_jffs2_fixup(struct mtd_info *mtd,
	struct nand_chip *chip, int page)
{
	struct mtk_nfc *nfc = nand_get_controller_data(chip);
	int ret;

	ret = mtk_nfc_pre_jffs2_fixup(mtd, chip, page);
	if (ret <= 0)
		return ret;

	/* write page with old data and new oob */
	ret = mtk_nfc_page_erase_write(mtd, chip, nfc->pending_page,
				       nfc->pending_oob[0], page);

	/* restore original oob data */
	memcpy(chip->oob_poi, nfc->pending_oob[0], mtd->oobsize);

	return !ret ? 1 : ret;
}

static int mtk_nfc_write_oob_std(struct mtd_info *mtd, struct nand_chip *chip,
	int page)
{
	struct mtk_nfc *nfc = nand_get_controller_data(chip);
	struct mtk_nfc_nand_chip *mtk_nand = to_mtk_nand(chip);
	int ret, raw = 0;

	if (mtk_nand->jffs2_fixup) {
		ret = mtk_nfc_write_oob_jffs2_fixup(mtd, chip, page);
		if (ret)
			return ret > 0 ? 0 : ret;
	}

	if (mtk_nfc_check_empty_page(mtd, chip, NULL)) {
		/* Do not write empty oob */
		if (mtk_nfc_check_empty_page_spare_trailing(mtd, chip))
			return 0;

		/* ecc-protected data is empty, use raw write */
		raw = 1;
	}

	memset(nfc->buffer, 0xff, mtd->writesize + mtd->oobsize);

	chip->cmdfunc(mtd, NAND_CMD_SEQIN, 0x00, page);

	if (raw)
		ret = _mtk_nfc_write_page_raw(mtd, chip, NULL, 1, page, 0);
	else
		ret = mtk_nfc_write_page_hwecc(mtd, chip, nfc->buffer, 1, page);

	if (ret < 0)
		return -EIO;

	chip->cmdfunc(mtd, NAND_CMD_PAGEPROG, -1, -1);
	ret = chip->waitfunc(mtd, chip);

	return ret & NAND_STATUS_FAIL ? -EIO : 0;
}

static void mtk_nfc_oneshot_read_buf(struct mtd_info *mtd,
				     struct nand_chip *chip, u8 *buf, u32 len)
{
	struct mtk_nfc *nfc = nand_get_controller_data(chip);
	u32 reg;

	nfi_set_reg16(nfc, CNFG_READ_EN, NFI_CNFG);
	nfi_clear_reg16(nfc, CNFG_AUTO_FMT_EN | CNFG_HW_ECC_EN, NFI_CNFG);
	reg = chip->ecc.steps << CON_SEC_SHIFT | CON_BRD;
	nfi_writew(nfc, reg, NFI_CON);

	mtk_nfc_read_buf(mtd, buf, len);

	nfi_writew(nfc, 0, NFI_CON);
}

static void mtk_nfc_read_page_ecc_trailings(struct mtd_info *mtd,
					    struct nand_chip *chip, bool ecc)
{
	struct mtk_nfc *nfc = nand_get_controller_data(chip);
	struct mtk_nfc_nand_chip *mtk_nand = to_mtk_nand(chip);
	u32 offs, len;
	int i;

	if (!ecc)
		goto trailing_only;

	/* Read out ecc parity code */
	for (i = 0; i < chip->ecc.steps; i++) {
		/* Offset of the ecc data of the sector in raw page */
		offs = (uintptr_t)mtk_ecc_ptr(chip, i) - (uintptr_t)nfc->buffer;

		len = mtk_nand->spare_per_sector - nfc->caps->fdm_size;

		/*
		 * The NAND cache still contains the page we've read.
		 * Use Random Data Out to read the ecc data directly
		 */
		chip->cmdfunc(mtd, NAND_CMD_RNDOUT, offs, -1);

		mtk_nfc_oneshot_read_buf(mtd, chip, ecc_ptr(chip, i), len);
	}

trailing_only:
	/* Read out trailing bytes */
	if (mtk_nand->trailing_bytes) {
		/* Offset of the trailing bytes can't be accessed by NFI */
		offs = mtd->oobsize - mtk_nand->trailing_bytes;

		chip->cmdfunc(mtd, NAND_CMD_RNDOUT, mtd->writesize + offs, -1);

		mtk_nfc_oneshot_read_buf(mtd, chip, chip->oob_poi + offs,
					 mtk_nand->trailing_bytes);
	}
}

static int mtk_nfc_read_page_hwecc(struct mtd_info *mtd, struct nand_chip *chip,
	u8 *buf, int oob_on, int page)
{
	struct mtk_nfc *nfc = nand_get_controller_data(chip);
	struct device *dev = nfc->dev;
	int bitflips[16], errsecs = 0, max_bitflips = 0;
	int ret = 0, rc, i;
	u32 reg;

	nfi_set_reg16(nfc, CNFG_READ_EN | CNFG_AUTO_FMT_EN
			| CNFG_HW_ECC_EN, NFI_CNFG);

	nfc->ecc_cfg.op = ECC_DECODE;
	mtk_ecc_init(nfc, nfc->ecc, &nfc->ecc_cfg);
	mtk_ecc_enable(nfc->ecc, &nfc->ecc_cfg);

	reg =  chip->ecc.steps << CON_SEC_SHIFT | CON_BRD;
	nfi_writew(nfc, reg, NFI_CON);

	/* Reset oob buffer to full 0xff */
	memset(chip->oob_poi, 0xff, mtd->oobsize);

	for (i = 0; i < chip->ecc.steps; i++) {
		mtk_nfc_read_buf(mtd, data_ptr(chip, buf, i), chip->ecc.size);
		rc = mtk_ecc_wait_decode_done(nfc->ecc, i);

		mtk_nfc_read_fdm(chip, i, 1);

		bitflips[i] = 0;

		if (rc < 0) {
			ret = -EIO;
		} else {
			rc = mtk_ecc_correct_check(mtd, nfc->ecc,
				data_ptr(chip, buf, i), oob_ptr(chip, i), i);

			if (rc < 0) {
				/* Record this sector */
				errsecs |= BIT(i);

				if (!ret)
					ret = -EBADMSG;
			} else if (rc) {
				bitflips[i] = rc;

				dev_info(dev,
					 "%u bitflip%s corrected at page %u setp %u\n",
					 rc,  rc > 1 ? "s" : "", page, i);
			}
		}
	}

	mtk_ecc_disable(nfc->ecc);
	nfi_writew(nfc, 0, NFI_CON);

	mtk_nfc_read_page_ecc_trailings(mtd, chip, true);

	if (ret != -EBADMSG)
		goto out;

	/* Start raw read */
	for (i = 0; i < chip->ecc.steps; i++) {
		if (!(errsecs & BIT(i)))
			continue;

		/* Try to fix the empty page */
		rc = mtk_ecc_fixup_empty_step(nfc->ecc, chip,
					      nfc->caps->fdm_size,
					      data_ptr(chip, buf, i),
					      oob_ptr(chip, i),
					      ecc_ptr(chip, i));
		if (rc >= 0) {
			errsecs &= ~BIT(i);
			bitflips[i] = rc;
			dev_info(dev,
				 "%u bitflip%s corrected at empty page %u step %u\n",
				 rc,  rc > 1 ? "s" : "", page, i);
		} else {
			dev_warn(dev,
				 "Uncorrectable bitflips in page %u, step %u\n",
				 page, i);
		}
	}

	if (!errsecs)
		ret = 0;

out:
	if (ret) {
		if (ret == -EBADMSG) {
			mtd->ecc_stats.failed++;
			return chip->ecc.strength + 1;
		}

		return ret;
	}

	for (i = 0; i < chip->ecc.steps; i++) {
		ret += bitflips[i];
		max_bitflips = max_t(unsigned int, max_bitflips, bitflips[i]);
	}

	mtd->ecc_stats.corrected += ret;

	return max_bitflips;
}

static int mtk_nfc_read_page_raw(struct mtd_info *mtd, struct nand_chip *chip,
				 u8 *buf, int oob_on, int page)
{
	struct mtk_nfc *nfc = nand_get_controller_data(chip);
	struct mtk_nfc_nand_chip *mtk_nand = to_mtk_nand(chip);
	int i;
	u32 reg;

	nfi_set_reg16(nfc, CNFG_READ_EN, NFI_CNFG);
	nfi_clear_reg16(nfc, CNFG_AUTO_FMT_EN | CNFG_HW_ECC_EN, NFI_CNFG);
	reg =  chip->ecc.steps << CON_SEC_SHIFT | CON_BRD;
	nfi_writew(nfc, reg, NFI_CON);

	memset(nfc->buffer, 0xff, mtd->writesize + mtd->oobsize);
	mtk_nfc_read_buf(mtd, nfc->buffer,
			 mtd->writesize + mtk_nand->oobsize_avail);
	nfi_writew(nfc, 0, NFI_CON);

	mtk_nfc_read_page_ecc_trailings(mtd, chip, false);

	for (i = 0; i < chip->ecc.steps; i++) {
		memcpy(oob_ptr(chip, i), mtk_oob_ptr(chip, i),
		       nfc->caps->fdm_size);
		memcpy(ecc_ptr(chip, i), mtk_ecc_ptr(chip, i),
		       mtk_nand->spare_per_sector - nfc->caps->fdm_size);

		if (buf)
			memcpy(data_ptr(chip, buf, i), mtk_data_ptr(chip, i),
			       chip->ecc.size);
	}

	return 0;
}

static int mtk_nfc_read_oob_raw(struct mtd_info *mtd, struct nand_chip *chip,
				int page)
{
	chip->cmdfunc(mtd, NAND_CMD_READ0, 0, page);

	return mtk_nfc_read_page_raw(mtd, chip, NULL, 1, page);
}

static int mtk_nfc_read_oob_std(struct mtd_info *mtd, struct nand_chip *chip,
	int page)
{
	int ret;
	struct mtk_nfc *nfc = nand_get_controller_data(chip);

	chip->cmdfunc(mtd, NAND_CMD_READ0, 0, page);

	ret = mtk_nfc_read_page_hwecc(mtd, chip, nfc->buffer, 1, page);
	if(ret) {
		dev_err(nfc->dev, "skip ECC at page %d\n", page);
		ret = mtk_nfc_read_oob_raw(mtd, chip, page);
	}

	return ret;
}

static inline void mtk_nfc_hw_init(struct mtk_nfc *nfc)
{
	/*
	 * CNRNB: nand ready/busy register
	 * -------------------------------
	 * 7:4: timeout register for polling the NAND busy/ready signal
	 * 0  : poll the status of the busy/ready signal after [7:4]*16 cycles.
	 */
	nfi_writew(nfc, 0xf1, NFI_CNRNB);
	nfi_writel(nfc, PAGEFMT_4K, NFI_PAGEFMT);

	mtk_nfc_hw_reset(nfc);

	nfi_readl(nfc, NFI_INTR_STA);
	nfi_writel(nfc, 0, NFI_INTR_EN);
}

static int mtk_nfc_ooblayout_free(struct mtd_info *mtd, int section,
				  struct mtd_oob_region *oob_region)
{
	struct nand_chip *chip = mtd_to_nand(mtd);
	struct mtk_nfc *nfc = nand_get_controller_data(chip);
	u32 eccsteps;

	eccsteps = mtd->writesize / chip->ecc.size;

	if (section >= eccsteps)
		return -ERANGE;

	oob_region->length = nfc->caps->fdm_size - 1;
	oob_region->offset = section * nfc->caps->fdm_size + 1;

	return 0;
}

static int mtk_nfc_ooblayout_ecc(struct mtd_info *mtd, int section,
				 struct mtd_oob_region *oob_region)
{
	struct nand_chip *chip = mtd_to_nand(mtd);
	struct mtk_nfc *nfc = nand_get_controller_data(chip);
	u32 eccsteps;

	if (section)
		return -ERANGE;

	eccsteps = mtd->writesize / chip->ecc.size;
	oob_region->offset = nfc->caps->fdm_size * eccsteps;
	oob_region->length = mtd->oobsize - oob_region->offset;

	return 0;
}

static const struct mtd_ooblayout_ops mtk_nfc_ooblayout_ops = {
	.free = mtk_nfc_ooblayout_free,
	.ecc = mtk_nfc_ooblayout_ecc,
};

static int mtk_nfc_ooblayout_ecc_spare_free(struct mtd_info *mtd, int section,
					  struct mtd_oob_region *oob_region)
{
	struct nand_chip *chip = mtd_to_nand(mtd);
	struct mtk_nfc_nand_chip *mtk_nand = to_mtk_nand(chip);

	if (section >= chip->ecc.steps)
		return -ERANGE;

	oob_region->length = mtk_nand->ecc_spare_bytes;
	oob_region->offset = (uintptr_t)ecc_ptr(chip, section) -
			     (uintptr_t)chip->oob_poi + chip->ecc.bytes;

	return 0;
}

static const struct mtd_ooblayout_ops mtk_nfc_ooblayout_ecc_spare_ops = {
	.free = mtk_nfc_ooblayout_ecc_spare_free,
	.ecc = mtk_nfc_ooblayout_ecc,
};

static int mtk_nfc_ooblayout_trailing_free(struct mtd_info *mtd, int section,
					   struct mtd_oob_region *oob_region)
{
	struct nand_chip *chip = mtd_to_nand(mtd);
	struct mtk_nfc_nand_chip *mtk_nand = to_mtk_nand(chip);

	if (section > 0)
		return -ERANGE;

	oob_region->length = mtk_nand->trailing_bytes;
	oob_region->offset = mtd->oobsize - mtk_nand->trailing_bytes;

	return 0;
}

static const struct mtd_ooblayout_ops mtk_nfc_ooblayout_trailing_ops = {
	.free = mtk_nfc_ooblayout_trailing_free,
	.ecc = mtk_nfc_ooblayout_ecc,
};

static int mtk_nfc_block_bad(struct mtd_info *mtd, loff_t ofs)
{
	struct nand_chip *chip = mtd_to_nand(mtd);
	int page, res = 0, i = 0;
	u16 bad;

	if (chip->bbt_options & NAND_BBT_SCANLASTPAGE)
		ofs += mtd->erasesize - mtd->writesize;

	page = (int) (ofs >> chip->page_shift) & chip->pagemask;

	do {
		chip->cmdfunc(mtd, NAND_CMD_READ0,
			chip->ecc.size + chip->badblockpos, page);

		bad = chip->read_byte(mtd);
		res = bad != 0xFF;

		ofs += mtd->writesize;
		page = (int) (ofs >> chip->page_shift) & chip->pagemask;
		i++;
	} while (!res && i < 2 && (chip->bbt_options & NAND_BBT_SCAN2NDPAGE));

	return res;
}

static int mtk_nfc_block_markbad(struct mtd_info *mtd, loff_t ofs)
{
	struct nand_chip *chip = mtd_to_nand(mtd);
	loff_t lofs;
	int page, ret = 0, res, i = 0;

	/* Create bad block mark OOB bata */
	memset(chip->oob_poi, 0xff, mtd->oobsize);
	chip->oob_poi[chip->badblockpos] = 0;

	/* For BootROM compatibility, always write to offset 0 */
	chip->oob_poi[0] = 0;

	/* Write to last page(s) if necessary */
	if (chip->bbt_options & NAND_BBT_SCANLASTPAGE) {
		lofs = ofs + mtd->erasesize - mtd->writesize;
		if (chip->bbt_options & NAND_BBT_SCAN2NDPAGE)
			lofs -= mtd->writesize;

		do {
			page = lofs >> chip->page_shift;
			res = mtk_nfc_write_oob_std(mtd, chip, page);
			if (!ret)
				ret = res;

			i++;
			lofs += mtd->writesize;
		} while ((chip->bbt_options & NAND_BBT_SCAN2NDPAGE) && i < 2);
	}

	/* For BootROM compatibility, always write to first page(s) */
	i = 0;
	do {
		page = ofs >> chip->page_shift;
		res = mtk_nfc_write_oob_std(mtd, chip, page);
		if (!ret)
			ret = res;

		i++;
		ofs += mtd->writesize;
	} while ((chip->bbt_options & NAND_BBT_SCAN2NDPAGE) && i < 2);

	return ret;
}

/*******************************************************************************
 * The following functions are used to solve JFFS2 incompatible issue.
 ******************************************************************************/

static int mtk_nfc_do_write_page(struct mtd_info *mtd, struct nand_chip *chip,
				 const uint8_t *buf, int page, int ecc_on)
{
	int status, write_ecc_protected = 1;

	if (ecc_on) {
		if (mtk_nfc_check_empty_page(mtd, chip, buf)) {
			/* Do not write empty page */
			if (mtk_nfc_check_empty_page_spare_trailing(mtd, chip))
				return 0;

			/* ecc-protected region is empty, use raw write */
			ecc_on = 0;
			write_ecc_protected = 0;
		}
	} else {
		/* Do not write full empty page */
		if (mtk_nfc_check_empty_page_full(mtd, chip, buf))
			return 0;
	}

	chip->cmdfunc(mtd, NAND_CMD_SEQIN, 0x00, page);

	if (ecc_on)
		status = mtk_nfc_write_page_hwecc(mtd, chip, buf, 1, page);
	else
		status = _mtk_nfc_write_page_raw(mtd, chip, buf, 1, page,
						 write_ecc_protected);

	if (status < 0)
		return status;

	chip->cmdfunc(mtd, NAND_CMD_PAGEPROG, -1, -1);

	status = chip->waitfunc(mtd, chip);
	if (status & NAND_STATUS_FAIL)
		return -EIO;

	return 0;
}

static int mtk_nfc_do_read_page(struct mtd_info *mtd, struct nand_chip *chip,
				uint8_t *buf, int page, int ecc_on)
{
	int status;

	chip->cmdfunc(mtd, NAND_CMD_READ0, 0x00, page);

	if (ecc_on)
		status = mtk_nfc_read_page_hwecc(mtd, chip, buf, 1, page);
	else
		status = mtk_nfc_read_page_raw(mtd, chip, buf, 1, page);

	if (status > chip->ecc.strength)
		status = -EBADMSG;

	if (status < 0)
		return status;

	return 0;
}

static int mtk_nfc_do_erase(struct mtd_info *mtd, struct nand_chip *chip,
	int page)
{
	int status;

	chip->cmdfunc(mtd, NAND_CMD_ERASE1, -1, page);
	chip->cmdfunc(mtd, NAND_CMD_ERASE2, -1, -1);

	status = chip->waitfunc(mtd, chip);
	if (status & NAND_STATUS_FAIL)
		return -EIO;

	return 0;
}

static int mtk_nfc_page_erase_write(struct mtd_info *mtd,
	struct nand_chip *chip, const uint8_t *buf, const uint8_t *oob,
	int page)
{
	struct mtk_nfc *nfc = nand_get_controller_data(chip);
	int pages_per_block, page_start;
	struct device *dev = nfc->dev;
	int i, ret;

	pages_per_block = mtd->erasesize / mtd->writesize;
	page_start = page - page % pages_per_block;

	/* read all pages within this block except the one to be rewritten */
	for (i = 0; i < pages_per_block; i++) {
		if (page_start + i == page)
			continue;

		ret = mtk_nfc_do_read_page(mtd, chip, nfc->block_buffer[i].buf,
					   page_start + i, 1);
		if (!ret) {
			/* no error, or ecc corrected */
			nfc->block_buffer[i].ecc_on = 1;
		} else if (ret == -EBADMSG) {
			/* unrecoverable ecc error. switch to raw read */
			ret = mtk_nfc_do_read_page(mtd, chip,
						   nfc->block_buffer[i].buf,
						   page_start + i, 0);
			if (ret) {
				/* I/O error. print error */
				dev_err(dev, "jffs2 fixup: raw read error ");
				pr_cont("%d on page [%u]\n", ret, page);
				nfc->block_buffer[i].ecc_on = -1;
			}
			nfc->block_buffer[i].ecc_on = 0;
		} else {
			/* I/O error. print error */
			dev_err(dev,
				"jffs2 fixup: read error %d on page [%u]\n",
				ret, page);
			nfc->block_buffer[i].ecc_on = -1;
		}

		memcpy(nfc->block_buffer[i].buf + mtd->writesize, chip->oob_poi,
		       mtd->oobsize);
	}

	/* erase this block */
	ret = mtk_nfc_do_erase(mtd, chip, page_start);
	if (ret) {
		/* erase failure. print error */
		dev_err(dev, "jffs2 fixup: erase failed %d on page [%u]\n",
			ret, page);
		return ret;
	}

	/* write back pages except the one to be rewritten */
	for (i = 0; i < pages_per_block; i++) {
		if (page_start + i == page)
			continue;

		/* skip write page which failed on reading */
		if (nfc->block_buffer[i].ecc_on < 0) {
			dev_info(dev,
				 "jffs2 fixup: skipping writing page [%u]\n",
				 page);
			continue;
		}

		memcpy(chip->oob_poi, nfc->block_buffer[i].buf + mtd->writesize,
		       mtd->oobsize);
		ret = mtk_nfc_do_write_page(mtd, chip, nfc->block_buffer[i].buf,
					    page_start + i,
					    nfc->block_buffer[i].ecc_on);
		if (ret) {
			dev_err(dev,
				"jffs2 fixup: write error %d on page [%u]\n",
				ret, page);
		}
	}

	/* write page */
	memcpy(chip->oob_poi, oob, nfc->caps->fdm_size * chip->ecc.steps);

	ret = mtk_nfc_do_write_page(mtd, chip, buf, page, 1);
	if (ret) {
		dev_err(dev, "jffs2 fixup: write error %d on page [%u]\n",
			ret, page);
	}

	return ret;
}

static int mtk_nfc_pre_jffs2_fixup(struct mtd_info *mtd,
				   struct nand_chip *chip, int page)
{
	struct mtk_nfc *nfc = nand_get_controller_data(chip);
	struct device *dev = nfc->dev;
	int ret, pages_per_block;

	/*
	 * only do this for first page of a block
	 * (i.e. the page with JFFS2 clean marker)
	 */
	pages_per_block = mtd->erasesize / mtd->writesize;
	if (page % pages_per_block)
		return 0;

	/* backup pending oob data */
	memcpy(nfc->pending_oob[0], chip->oob_poi, mtd->oobsize);

	/* read target page in ecc mode first to check whether it's empty */
	ret = mtk_nfc_do_read_page(mtd, chip, nfc->pending_page, page, 1);
	if (ret) {
		if (ret != -EBADMSG) {
			/* read failure */
			dev_err(dev,
				"jffs2 fixup: raw read error %d on page [%u]\n",
				ret, page);
		} else {
			/* page has unrecoverable ecc error */
			dev_err(dev, "jffs2 fixup: rejected due to ");
			pr_cont("uncorrectable bitflips on page [%u]\n", page);
		}

		/* reject follow-up actions and restore pending oob data */
		memcpy(chip->oob_poi, nfc->pending_oob[0], mtd->oobsize);
		return ret;
	}

	/* page is ecc protected, check if the whole page is empty */
	if (mtk_nfc_check_empty_page_full(mtd, chip, nfc->pending_page)) {
		/* page is empty. do normal writing */
		memcpy(chip->oob_poi, nfc->pending_oob[0], mtd->oobsize);
		return 0;
	}

	return 1;
}

static int mtk_nfc_write_page_jffs2_fixup(struct mtd_info *mtd,
	struct nand_chip *chip, const uint8_t *buf, int page)
{
	struct mtk_nfc *nfc = nand_get_controller_data(chip);
	int ret;

	ret = mtk_nfc_pre_jffs2_fixup(mtd, chip, page);
	if (ret <= 0)
		return ret;

	/* backup pending page data (buf will be touched during write) */
	memcpy(nfc->pending_page, buf, mtd->writesize);

	/* backup in-flash oob data */
	memcpy(nfc->pending_oob[1], chip->oob_poi, mtd->oobsize);

	/* write page with new data and old oob */
	ret = mtk_nfc_page_erase_write(mtd, chip, nfc->pending_page,
				       nfc->pending_oob[1], page);

	/* restore original oob */
	memcpy(chip->oob_poi, nfc->pending_oob[0], mtd->oobsize);

	return !ret ? 1 : ret;
}

static int mtk_nfc_write_page(struct mtd_info *mtd, struct nand_chip *chip,
	uint32_t offset, int data_len, const uint8_t *buf,
	int oob_required, int page, int cached, int raw)
{
	int status, write_ecc_protected = 1;

	if (likely(!raw)) {
		status = mtk_nfc_write_page_jffs2_fixup(mtd, chip, buf, page);
		if (status)
			return status > 0 ? 0 : status;
	}

	if (likely(!raw)) {
		if (mtk_nfc_check_empty_page(mtd, chip, buf)) {
			/* Do not write empty page */
			if (mtk_nfc_check_empty_page_spare_trailing(mtd, chip))
				return 0;

			/* ecc-protected region is empty, use raw write */
			raw = 1;
			write_ecc_protected = 0;
		}
	} else {
		/* Do not write full empty page */
		if (mtk_nfc_check_empty_page_full(mtd, chip, buf))
			return 0;
	}

	chip->cmdfunc(mtd, NAND_CMD_SEQIN, 0x00, page);

	if (unlikely(raw))
		status = _mtk_nfc_write_page_raw(mtd, chip, buf,
			oob_required, page, write_ecc_protected);
	else
		status = mtk_nfc_write_page_hwecc(mtd, chip, buf,
			oob_required, page);

	if (status < 0)
		return status;

	chip->cmdfunc(mtd, NAND_CMD_PAGEPROG, -1, -1);

	status = chip->waitfunc(mtd, chip);
	if (status & NAND_STATUS_FAIL)
		return -EIO;

	return 0;
}

static int mtk_nfc_nand_chip_init(struct device *dev, struct mtk_nfc *nfc,
				  struct device_node *np)
{
	struct mtk_nfc_nand_chip *chip;
	struct nand_chip *nand;
	struct mtd_info *mtd;
	struct mtd_part_parser_data ppdata;
	struct device_node *ofpart_node;
	int nsels, len, npgs, ret, i;
	bool raw_oob;
	u32 tmp;

	raw_oob = of_property_read_bool(np, "mediatek,raw-oob-layout");

	if (!of_get_property(np, "reg", &nsels))
		return -ENODEV;

	nsels /= sizeof(u32);
	if (!nsels || nsels > MTK_NAND_MAX_NSELS) {
		dev_err(dev, "invalid reg property size %d\n", nsels);
		return -EINVAL;
	}

	chip = devm_kzalloc(dev, sizeof(*chip) + nsels * sizeof(u8),
			    GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->nsels = nsels;
	for (i = 0; i < nsels; i++) {
		ret = of_property_read_u32_index(np, "reg", i, &tmp);
		if (ret) {
			dev_err(dev, "reg property failure : %d\n", ret);
			return ret;
		}
		chip->sels[i] = tmp;
	}

	chip->jffs2_fixup = of_property_read_bool(np, "mediatek,jffs2-fixup");

	nand = &chip->nand;
	nand->controller = &nfc->controller;

	nand_set_flash_node(nand, np);
	nand_set_controller_data(nand, nfc);

	nand->options |= NAND_USE_BOUNCE_BUFFER | NAND_NO_SUBPAGE_WRITE;
	nand->dev_ready = mtk_nfc_dev_ready;
	nand->select_chip = mtk_nfc_select_chip;
	nand->write_byte = mtk_nfc_write_byte;
	nand->write_buf = mtk_nfc_write_buf;
	nand->read_byte = mtk_nfc_read_byte;
	nand->read_buf = mtk_nfc_read_buf;
	nand->cmd_ctrl = mtk_nfc_cmd_ctrl;
	nand->block_bad = mtk_nfc_block_bad;
	nand->block_markbad = mtk_nfc_block_markbad;

	/* set default mode in case dt entry is missing */
	nand->ecc.mode = NAND_ECC_HW;

	nand->ecc.write_page_raw = mtk_nfc_write_page_raw;
	nand->ecc.write_page = mtk_nfc_write_page_hwecc;
	nand->ecc.write_oob_raw = mtk_nfc_write_oob_raw;
	nand->ecc.write_oob = mtk_nfc_write_oob_std;

	nand->ecc.read_page_raw = mtk_nfc_read_page_raw;
	nand->ecc.read_page = mtk_nfc_read_page_hwecc;
	nand->ecc.read_oob_raw = mtk_nfc_read_oob_raw;
	nand->ecc.read_oob = mtk_nfc_read_oob_std;

	mtd = nand_to_mtd(nand);
	mtd->owner = THIS_MODULE;
	mtd->dev.parent = dev;
	mtd->name = MTK_NAME;
	mtd_set_ooblayout(mtd, &mtk_nfc_ooblayout_ops);

	mtk_nfc_hw_init(nfc);

	ret = nand_scan_ident(mtd, nsels, NULL);
	if (ret)
		return ret;

	/* store bbt magic in page, cause OOB is not protected */
	if (nand->bbt_options & NAND_BBT_USE_FLASH)
		nand->bbt_options |= NAND_BBT_NO_OOB;

	if (nand->options & NAND_BUSWIDTH_16) {
		dev_err(dev, "16bits buswidth not supported");
		return -EINVAL;
	}

	nand->select_chip(mtd, 0);

	/* Dedicated case for handling raw oob */
	if (!raw_oob) {
		if (chip->jffs2_fixup) {
			nand->write_page = mtk_nfc_write_page;
			dev_info(dev, "JFFS2 fixup enabled\n");
		}
	} else if (chip->trailing_bytes) {
		mtd_set_ooblayout(mtd, &mtk_nfc_ooblayout_trailing_ops);
		nand->ecc.write_oob = mtk_nfc_write_oob_raw;
		nand->ecc.read_oob = mtk_nfc_read_oob_raw;

		dev_info(dev, "chip has %u bytes uncovered by NFI\n",
			 chip->trailing_bytes);
	} else if (chip->ecc_spare_bytes) {
		mtd_set_ooblayout(mtd, &mtk_nfc_ooblayout_ecc_spare_ops);
		nand->ecc.write_oob = mtk_nfc_write_oob_raw;
		nand->ecc.read_oob = mtk_nfc_read_oob_raw;

		dev_info(dev, "chip has %u spare byte(s) in ecc per step\n",
			 chip->ecc_spare_bytes);
	} else {
		if (chip->jffs2_fixup)
			nand->write_page = mtk_nfc_write_page;
		dev_warn(dev, "raw oob layout not available\n");
	}

	len = mtd->writesize + mtd->oobsize;
	nfc->buffer = devm_kzalloc(dev, len, GFP_KERNEL);
	if (!nfc->buffer)
		return -ENOMEM;

	npgs = mtd->erasesize / mtd->writesize;
	nfc->block_buffer = devm_kzalloc(dev, npgs * sizeof(*nfc->block_buffer), GFP_KERNEL);
	if (!nfc->block_buffer)
		return -ENOMEM;

	for (i = 0; i < npgs; i++) {
		nfc->block_buffer[i].buf = devm_kzalloc(dev, len, GFP_KERNEL);
		if (!nfc->block_buffer[i].buf)
			return -ENOMEM;
	}

	nfc->pending_page = devm_kzalloc(dev, mtd->writesize, GFP_KERNEL);
	if (!nfc->pending_page)
		return -ENOMEM;

	for (i = 0; i < ARRAY_SIZE(nfc->pending_oob); i++) {
		nfc->pending_oob[i] = devm_kzalloc(dev, mtd->oobsize, GFP_KERNEL);
		if (!nfc->pending_oob[i])
			return -ENOMEM;
	}

	ret = nand_scan_tail(mtd);
	if (ret)
		return ret;

	ppdata.of_node = of_get_next_available_child(dev->of_node, NULL);
	if (!ppdata.of_node) {
		dev_err(dev, "no nand device to configure\n");
		return -ENODEV;
	}

	ofpart_node = of_get_child_by_name(ppdata.of_node, "partitions");
	if (ofpart_node) {
		static const char * const probes[] = {"ofpart", NULL};

		ret = mtd_device_parse_register(mtd, probes, &ppdata, NULL, 0);
	}

	if (ret) {
		dev_err(dev, "mtd parse partition error\n");
		nand_release(mtd);
		return ret;
	}

	list_add_tail(&chip->node, &nfc->chips);

	return 0;
}

static int mtk_nfc_nand_chips_init(struct device *dev, struct mtk_nfc *nfc)
{
	struct device_node *np = dev->of_node;
	struct device_node *nand_np;
	int ret;

	for_each_child_of_node(np, nand_np) {
		ret = mtk_nfc_nand_chip_init(dev, nfc, nand_np);
		if (ret) {
			of_node_put(nand_np);
			return ret;
		}
	}

	return 0;
}

static const struct mtk_nfc_caps mtk_nfc_caps_mt7621 = {
	.pageformat_spare_shift = 4,
	.max_sector = 8,
	.sector_size = 512,
	.fdm_size = 8,
	.fdm_ecc_size = 8,
};

static const struct of_device_id mtk_nfc_id_table[] = {
	{
		.compatible = "mediatek,mt7621-nfc",
		.data = &mtk_nfc_caps_mt7621,
	},
	{}
};
MODULE_DEVICE_TABLE(of, mtk_nfc_id_table);

static int mtk_nfc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct mtk_nfc *nfc;
	struct resource *res;
	const struct of_device_id *of_nfc_id = NULL;
	int ret;

	nfc = devm_kzalloc(dev, sizeof(*nfc), GFP_KERNEL);
	if (!nfc)
		return -ENOMEM;

	spin_lock_init(&nfc->controller.lock);
	init_waitqueue_head(&nfc->controller.wq);
	INIT_LIST_HEAD(&nfc->chips);

	/* probe defer if not ready */
	nfc->ecc = of_mtk_ecc_get(np);
	if (IS_ERR(nfc->ecc))
		return PTR_ERR(nfc->ecc);
	else if (!nfc->ecc)
		return -ENODEV;

	nfc->dev = dev;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	nfc->regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(nfc->regs)) {
		ret = PTR_ERR(nfc->regs);
		goto release_ecc;
	}

	of_nfc_id = of_match_device(mtk_nfc_id_table, &pdev->dev);
	if (!of_nfc_id) {
		ret = -ENODEV;
		goto release_ecc;
	}

	nfc->caps = of_nfc_id->data;

	platform_set_drvdata(pdev, nfc);

	ret = mtk_nfc_nand_chips_init(dev, nfc);
	if (ret) {
		dev_err(dev, "failed to init nand chips\n");
		goto release_ecc;
	}

	return 0;

release_ecc:
	mtk_ecc_release(nfc->ecc);

	return ret;
}

static int mtk_nfc_remove(struct platform_device *pdev)
{
	struct mtk_nfc *nfc = platform_get_drvdata(pdev);
	struct mtk_nfc_nand_chip *chip;

	while (!list_empty(&nfc->chips)) {
		chip = list_first_entry(&nfc->chips, struct mtk_nfc_nand_chip,
					node);
		nand_release(nand_to_mtd(&chip->nand));
		list_del(&chip->node);
	}

	mtk_ecc_release(nfc->ecc);

	return 0;
}

static struct platform_driver mtk_nfc_driver = {
	.probe  = mtk_nfc_probe,
	.remove = mtk_nfc_remove,
	.driver = {
		.name  = MTK_NAME,
		.of_match_table = mtk_nfc_id_table,
	},
};

module_platform_driver(mtk_nfc_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Xiangsheng Hou <xiangsheng.hou@mediatek.com>");
MODULE_AUTHOR("Weijie Gao <weijie.gao@mediatek.com>");
MODULE_DESCRIPTION("MTK Nand Flash Controller Driver");
