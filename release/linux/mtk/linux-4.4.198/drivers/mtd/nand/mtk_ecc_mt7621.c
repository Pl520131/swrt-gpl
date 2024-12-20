/*
 * SPDX-License-Identifier: GPL-2.0
 * Driver for MediaTek NFI ECC controller
 *
 * Copyright (C) 2018 MediaTek Inc.
 * Authors:	Xiangsheng Hou	<xiangsheng.hou@mediatek.com>
 *		Weijie Gao	<weijie.gao@mediatek.com>
 *
 */

#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/clk.h>
#include <linux/module.h>
#include <linux/iopoll.h>
#include <linux/bitops.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/mutex.h>

#include "mtk_nand_mt7621.h"

static const u8 ecc_strength_mt7621[] = {
	4, 6, 8, 10, 12
};

static inline void mtk_ecc_wait_idle(struct mtk_ecc *ecc,
				     enum mtk_ecc_operation op)
{
	struct device *dev = ecc->dev;
	u32 val;
	int ret;

	ret = readl_poll_timeout_atomic(ecc->regs + ECC_IDLE_REG(op), val,
					val & ECC_IDLE_MASK,
					10, MTK_TIMEOUT);
	if (ret)
		dev_warn(dev, "%s NOT idle\n",
			 op == ECC_ENCODE ? "encoder" : "decoder");
}

int mtk_ecc_correct_check(struct mtd_info *mtd, struct mtk_ecc *ecc,
			  u8 *sector_buf, u8 *fdm_buf, u32 sector_index)
{
	struct nand_chip *nand = mtd_to_nand(mtd);
	struct mtk_nfc *nfc = nand_get_controller_data(nand);
	u32 error_byte_pos, error_bit_pos_in_byte;
	u32 error_locations, error_bit_loc;
	u32 num_error_bits;
	int bitflips = 0;
	u32 i;

	if(fdm_buf[0] == 0x00) {
		dev_info(ecc->dev, "skip ecc on bad block\n");
		return 0;
	}

	num_error_bits = (readl(ecc->regs + ECC_DECENUM)
		>> (sector_index << 2)) & ecc->caps->err_mask;
	if (!num_error_bits)
		return 0;

	if (num_error_bits == ecc->caps->err_mask)
		return -1;

	for (i = 0; i < num_error_bits; i++) {
		error_locations = readl(ecc->regs + ECC_DECEL(i / 2));
		error_bit_loc = (error_locations >> ((i % 2) * DEC_EL_SHIFT))
				 & DEC_EL_MASK;
		error_byte_pos = error_bit_loc >> DEC_EL_BYTE_SHIFT;
		error_bit_pos_in_byte = error_bit_loc & DEC_EL_BIT_MASK;

		if (error_bit_loc < (nand->ecc.size << 3)) {
			sector_buf[error_byte_pos] ^=
				(1 << error_bit_pos_in_byte);
		} else if (error_bit_loc <
			((nand->ecc.size + nfc->caps->fdm_size) << 3)) {
			fdm_buf[error_byte_pos - nand->ecc.size] ^=
				(1 << error_bit_pos_in_byte);
		}

		bitflips++;
	}

	return bitflips;
}
EXPORT_SYMBOL(mtk_ecc_correct_check);


void mtk_ecc_release(struct mtk_ecc *ecc)
{
	put_device(ecc->dev);
}
EXPORT_SYMBOL(mtk_ecc_release);

static void mtk_ecc_hw_init(struct mtk_ecc *ecc)
{
	mtk_ecc_wait_idle(ecc, ECC_ENCODE);
	writew(ECC_OP_DISABLE, ecc->regs + ECC_ENCCON);

	mtk_ecc_wait_idle(ecc, ECC_DECODE);
	writel(ECC_OP_DISABLE, ecc->regs + ECC_DECCON);
}

static struct mtk_ecc *mtk_ecc_get(struct device_node *np)
{
	struct platform_device *pdev;
	struct mtk_ecc *ecc;

	pdev = of_find_device_by_node(np);
	if (!pdev || !platform_get_drvdata(pdev))
		return ERR_PTR(-EPROBE_DEFER);

	get_device(&pdev->dev);
	ecc = platform_get_drvdata(pdev);

	mtk_ecc_hw_init(ecc);

	return ecc;
}

struct mtk_ecc *of_mtk_ecc_get(struct device_node *of_node)
{
	struct mtk_ecc *ecc = NULL;
	struct device_node *np;

	np = of_parse_phandle(of_node, "ecc-engine", 0);
	if (np) {
		ecc = mtk_ecc_get(np);
		of_node_put(np);
	}

	return ecc;
}
EXPORT_SYMBOL(of_mtk_ecc_get);

int mtk_ecc_enable(struct mtk_ecc *ecc, struct mtk_ecc_config *config)
{
	enum mtk_ecc_operation op = config->op;

	mtk_ecc_wait_idle(ecc, op);
	writew(ECC_OP_ENABLE, ecc->regs + ECC_CTL_REG(op));

	return 0;
}
EXPORT_SYMBOL(mtk_ecc_enable);

void mtk_ecc_disable(struct mtk_ecc *ecc)
{
	enum mtk_ecc_operation op = ECC_ENCODE;

	/* find out the running operation */
	if (readw(ecc->regs + ECC_CTL_REG(op)) != ECC_OP_ENABLE)
		op = ECC_DECODE;

	/* disable it */
	mtk_ecc_wait_idle(ecc, op);
	writew(ECC_OP_DISABLE, ecc->regs + ECC_CTL_REG(op));
}
EXPORT_SYMBOL(mtk_ecc_disable);

int mtk_ecc_wait_decode_done(struct mtk_ecc *ecc, u32 sector_index)
{
	u32 val;
	int rc;

	rc = readb_poll_timeout_atomic(ecc->regs + ECC_DECDONE, val,
				  (val & (1 << sector_index)), 10, MTK_TIMEOUT);
	if (rc < 0) {
		dev_err(ecc->dev, "decode timeout\n");
		return -ETIMEDOUT;

	}

	return 0;
}
EXPORT_SYMBOL(mtk_ecc_wait_decode_done);

int mtk_ecc_init(struct mtk_nfc *nfc, struct mtk_ecc *ecc,
		 struct mtk_ecc_config *config)
{
	u32 ecc_bit, dec_sz, enc_sz;
	u32 reg, i;

	for (i = 0; i < ecc->caps->num_ecc_strength; i++) {
		if (ecc->caps->ecc_strength[i] == config->strength)
			break;
	}

	if (i == ecc->caps->num_ecc_strength) {
		dev_err(ecc->dev, "invalid ecc strength %d\n",
			config->strength);
		return -EINVAL;
	}

	ecc_bit = i;

	if (config->op == ECC_ENCODE) {
		/* configure ECC encoder (in bits) */
		enc_sz = config->len << 3;

		reg = ecc_bit | (ECC_NFI_MODE << ecc->caps->ecc_mode_shift);
		reg |= (enc_sz << ECC_MS_SHIFT);
		writel(reg, ecc->regs + ECC_ENCCNFG);
		writel(0, ecc->regs + ECC_ENCCON);

	} else {
		/* configure ECC decoder (in bits) */
		dec_sz = (config->len << 3) +
			 config->strength * ecc->caps->parity_bits;

		reg = ecc_bit | (ECC_NFI_MODE << ecc->caps->ecc_mode_shift);
		reg |= (dec_sz << ECC_MS_SHIFT) | DEC_CNFG_EL;
		reg |= DEC_EMPTY_EN;
		writel(reg, ecc->regs + ECC_DECCNFG);
		writel(0, ecc->regs + ECC_DECCON);
	}

	/* setup FDM register base */
	writel(CPHYSADDR((u32) nfc->regs + NFI_FDML(0)),
		ecc->regs + ECC_FDMADDR);

	return 0;
}
EXPORT_SYMBOL(mtk_ecc_init);

/* Empty page bitflip fixup */
static int mtk_ecc_check_bitflips(const void *buf, size_t len, u32 bitflips,
				  u32 bitflips_threshold)
{
	const u8 *buf8 = buf;
	const ulong *bufl;
	ulong d;
	u32 weight;

	while (len && ((uintptr_t)buf8) % sizeof(ulong)) {
		weight = hweight8(*buf8);
		bitflips += BITS_PER_BYTE - weight;
		buf8++;
		len--;

		if (unlikely(bitflips > bitflips_threshold))
			return -EBADMSG;
	}

	bufl = (const ulong *)buf8;
	while (len >= sizeof(ulong)) {
		d = *bufl;

		if (d != ~0UL) {
			weight = hweight_long(d);
			bitflips += BITS_PER_LONG - weight;
		}

		bufl++;
		len -= sizeof(ulong);

		if (unlikely(bitflips > bitflips_threshold))
			return -EBADMSG;
	}

	buf8 = (const u8 *)bufl;
	while (len) {
		weight = hweight8(*buf8);
		bitflips += BITS_PER_BYTE - weight;
		buf8++;
		len--;

		if (unlikely(bitflips > bitflips_threshold))
			return -EBADMSG;
	}

	return bitflips;
}

static int mtk_ecc_check_ecc_bitflips(const void *buf, u32 bits, u32 bitflips,
				      u32 bitflips_threshold)
{
	u32 len, i;
	int rc;
	u8 b;

	len = bits >> 3;
	bits &= 7;

	rc = mtk_ecc_check_bitflips(buf, len, bitflips, bitflips_threshold);
	if (!bits || rc < 0)
		return rc;

	bitflips = rc;

	/* We want a precise count of bits */
	b = ((const u8 *)buf)[len];
	for (i = 0; i < bits; i++) {
		if (!(b & BIT(i)))
			bitflips++;
	}

	if (unlikely(bitflips > bitflips_threshold))
		return -EBADMSG;

	return bitflips;
}

static void mtk_ecc_reset_ecc_bits(void *buf, u32 bits)
{
	u32 len;

	len = bits >> 3;
	bits &= 7;

	memset(buf, 0xff, len);

	/* Only reset bits protected by ECC to 1 */
	if (bits)
		((u8 *)buf)[len] |= GENMASK(bits - 1, 0);
}

int mtk_ecc_fixup_empty_step(struct mtk_ecc *ecc, struct nand_chip *chip,
			     u32 fdm_size, void *buf, void *oob, void *eccp)
{
	u32 ecc_bits = chip->ecc.strength * ecc->caps->parity_bits;
	int bitflips = 0;

	/*
	 * Check whether DATA + FDM + ECC of a sector contains correctable
	 * bitflips
	 */
	bitflips = mtk_ecc_check_bitflips(buf, chip->ecc.size, bitflips,
					  chip->ecc.strength);
	if (bitflips < 0)
		return -EBADMSG;

	bitflips = mtk_ecc_check_bitflips(oob, fdm_size,
					  bitflips, chip->ecc.strength);
	if (bitflips < 0)
		return -EBADMSG;

	bitflips = mtk_ecc_check_ecc_bitflips(eccp, ecc_bits,
					      bitflips, chip->ecc.strength);
	if (bitflips < 0)
		return -EBADMSG;

	if (!bitflips)
		return 0;

	/* Reset the data of this sector to 0xff */
	memset(buf, 0xff, chip->ecc.size);
	memset(oob, 0xff, fdm_size);
	mtk_ecc_reset_ecc_bits(eccp, ecc_bits);

	return bitflips;
}
EXPORT_SYMBOL(mtk_ecc_fixup_empty_step);

static const struct mtk_ecc_caps mtk_ecc_caps_mt7621 = {
	.err_mask = 0xf,
	.ecc_strength = ecc_strength_mt7621,
	.num_ecc_strength = ARRAY_SIZE(ecc_strength_mt7621),
	.ecc_mode_shift = 4,
	.parity_bits = 13,
};

static const struct of_device_id mtk_ecc_dt_match[] = {
	{
		.compatible = "mediatek,mt7621-ecc",
		.data = &mtk_ecc_caps_mt7621,
	},
	{},
};

static int mtk_ecc_probe(struct platform_device *pdev)
{
	const struct of_device_id *of_ecc_id = NULL;
	struct device *dev = &pdev->dev;
	struct mtk_ecc *ecc;
	struct resource *res;

	ecc = devm_kzalloc(dev, sizeof(*ecc), GFP_KERNEL);
	if (!ecc)
		return -ENOMEM;

	of_ecc_id = of_match_device(mtk_ecc_dt_match, &pdev->dev);
	if (!of_ecc_id)
		return -ENODEV;

	ecc->caps = of_ecc_id->data;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	ecc->regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(ecc->regs)) {
		dev_err(dev, "failed to map regs: %ld\n", PTR_ERR(ecc->regs));
		return PTR_ERR(ecc->regs);
	}

	ecc->dev = dev;
	platform_set_drvdata(pdev, ecc);

	return 0;
}

MODULE_DEVICE_TABLE(of, mtk_ecc_dt_match);

static struct platform_driver mtk_ecc_driver = {
	.probe  = mtk_ecc_probe,
	.driver = {
		.name  = "mtk-ecc",
		.of_match_table = of_match_ptr(mtk_ecc_dt_match),
	},
};

module_platform_driver(mtk_ecc_driver);

MODULE_AUTHOR("Xiangsheng Hou <xiangsheng.hou@mediatek.com>");
MODULE_AUTHOR("Weijie Gao <weijie.gao@mediatek.com>");
MODULE_DESCRIPTION("MTK Nand ECC Driver");
MODULE_LICENSE("GPL");
