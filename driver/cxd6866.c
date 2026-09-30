// SPDX-License-Identifier: GPL-2.0-only
/*
 * Sony CXD6866AER (FREIA) tuner driver (cxd6866.c)
 *
 * The CXD6866AER is the terrestrial/satellite tuner LSI used in the
 * XIT-SQR100 (USB VID 0x06B8, PID 0x106B).
 *
 * The register sequences implemented here are ported from the Sony FREIA
 * reference driver (docs/terr_cable_sat_freia/refcode/sony_freia.c,
 * "Based on FREIA application note 1.2.0"), which is the same silicon the
 * XIT-SQR100 Windows driver (IT9300BDA.sys) drives through the
 * sony_cxd2856_tuner_freia_* API.  The board-specific choices were taken
 * from IT9300BDA.sys:
 *
 *   - config flags 0x10004000:
 *       SONY_FREIA_CONFIG_LOOPFILTER_INTERNAL (0x10000000)
 *       SONY_FREIA_CONFIG_OUTLMT_DTV_1_2Vpp  (0x00004000)
 *   - tuner I2C 7-bit address 0x60 (8-bit form 0xC0)
 *   - 24 MHz crystal (register 0x81 = 0x18 in the binary's X_pon)
 *   - XOSC_SEL = 0x04 (100 uA), XOSC_CAP_SET = 0x30 (12 pF)
 *   - ISDB-S symbol rate fixed at 28860 ksps
 *   - ISDB-T 6 MHz parameter set (from g_terr_param_table, SONY_FREIA_DTV_ISDBT_6)
 *
 * The chip is detected at init time by reading register 0x7F:
 *   (value & 0xFC) == 0xF8  ->  CXD6866AER (FREIA)
 *   (value & 0xFC) == 0xF0  ->  CXD6868ER  (FREIA Plus)
 *   (value & 0xFC) == 0xF4  ->  CXD6866ER  (ASCOT4)
 * Only CXD6866AER / CXD6868ER are accepted here (ASCOT4 has no satellite
 * support in the reference driver and a different IF/AGC pin scheme).
 *
 * The FREIA part has no standalone LNA enable bit; the RFIN/LNA state is
 * selected through the power-save mode (SAT_NORMAL /
 * NORMAL_MATCHING_DISABLE), so there is no per-band LNA flag.
 *
 * Still not verifiable from the Windows driver (see
 * docs/20260926_xitsqr100_implementation_summary.md): the IT9303 GPIO
 * power pins and polarity, which are board-level and outside the
 * tuner I2C register map.  These must be confirmed on the target board.
 */

#include "print_format.h"
#include "cxd6866.h"

#ifdef __linux__
#include <linux/delay.h>
#endif

#define CXD6866_CHIP_ID_6866AER	0xf8	/* 0x7F & 0xFC */
#define CXD6866_CHIP_ID_6868ER	0xf0	/* 0x7F & 0xFC */

static int cxd6866_read_regs(struct cxd6866_tuner *tuner,
			     u8 reg, u8 *buf, int len)
{
	u8 b;
	struct i2c_comm_request req[2];

	if (!buf || !len)
		return -EINVAL;

	b = reg;

	req[0].req = I2C_WRITE_REQUEST;
	req[0].addr = tuner->i2c_addr;
	req[0].data = &b;
	req[0].len = 1;

	req[1].req = I2C_READ_REQUEST;
	req[1].addr = tuner->i2c_addr;
	req[1].data = buf;
	req[1].len = len;

	return i2c_comm_master_request(tuner->i2c, req, 2);
}

static int cxd6866_read_reg(struct cxd6866_tuner *tuner, u8 reg, u8 *val)
{
	return cxd6866_read_regs(tuner, reg, val, 1);
}

static int cxd6866_write_regs(struct cxd6866_tuner *tuner,
			      u8 reg, u8 *buf, int len)
{
	u8 b[255];
	struct i2c_comm_request req[1];

	if (!buf || !len || len > 254)
		return -EINVAL;

	b[0] = reg;
	memcpy(&b[1], buf, len);

	req[0].req = I2C_WRITE_REQUEST;
	req[0].addr = tuner->i2c_addr;
	req[0].data = b;
	req[0].len = 1 + len;

	return i2c_comm_master_request(tuner->i2c, req, 1);
}

static int cxd6866_write_reg(struct cxd6866_tuner *tuner, u8 reg, u8 val)
{
	return cxd6866_write_regs(tuner, reg, &val, 1);
}

static int cxd6866_write_reg_mask(struct cxd6866_tuner *tuner,
				  u8 reg, u8 val, u8 mask)
{
	int ret = 0;
	u8 tmp;

	if (!mask)
		return -EINVAL;

	if (mask != 0xff) {
		ret = cxd6866_read_regs(tuner, reg, &tmp, 1);
		if (ret)
			return ret;

		tmp &= ~mask;
		tmp |= (val & mask);
	} else {
		tmp = val;
	}

	return cxd6866_write_regs(tuner, reg, &tmp, 1);
}

/*
 * Detect the chip by reading register 0x7F.
 * Returns 0 if the device is a CXD6866AER / CXD6868ER, -ENODEV otherwise.
 */
static int cxd6866_detect(struct cxd6866_tuner *tuner)
{
	int ret;
	u8 val;

	ret = cxd6866_read_reg(tuner, 0x7f, &val);
	if (ret)
		return ret;

	/* The reference driver identifies the chip with (value & 0xFC). */
	switch (val & 0xfc) {
	case CXD6866_CHIP_ID_6868ER: /* CXD6868ER (FREIA Plus) */
	case CXD6866_CHIP_ID_6866AER: /* CXD6866AER (FREIA) */
		dev_dbg(tuner->dev, "cxd6866: detected 0x7F=0x%02x (FREIA family)\n",
			val);
		return 0;
	default:
		dev_err(tuner->dev,
			"cxd6866: unsupported chip 0x7F=0x%02x (expected 0xF0/0xF8)\n",
			val);
		return -ENODEV;
	}
}

/*
 * X_pon: power-on / reset sequence from the Sony reference.
 *
 * Uses the XIT-SQR100 config:
 *   - power save (terrestrial)  = NORMAL_MATCHING_DISABLE  (default)
 *   - power save (satellite)    = SAT_NORMAL               (default)
 *   - no REFOUT, no EXT_REF, no IFOUT_DC_BIAS_500mV, no SAT_LOW_GAIN
 *   - crystal frequency from config.xtal (XIT-SQR100: 24 MHz -> 0x18)
 *   - crystal driver current XOSC_SEL = 0x04 (100 uA)
 *   - crystal load cap XOSC_CAP_SET = 0x30 (12 pF, per IT9300BDA.sys)
 */
static int cxd6866_x_pon(struct cxd6866_tuner *tuner)
{
	int ret;
	u8 data[20];
	u8 tmp;

	/* Mode select (0x01): terrestrial */
	ret = cxd6866_write_reg(tuner, 0x01, 0x00);
	if (ret)
		return ret;

	/* RFIN matching in power save (terrestrial) (0x67):
	 * NORMAL_MATCHING_DISABLE -> 0x00 */
	ret = cxd6866_write_reg(tuner, 0x67, 0x00);
	if (ret)
		return ret;

	/* RFIN matching in power save (satellite) (0x43): SAT_NORMAL -> 0xC0 */
	ret = cxd6866_write_reg(tuner, 0x43, 0xc0);
	if (ret)
		return ret;

	/* Power save setting for analog block (0x5E, 0x5F, 0x60):
	 * NORMAL_MATCHING_DISABLE -> {0x15, 0x00, 0x00} */
	data[0] = 0x15;
	data[1] = 0x00;
	data[2] = 0x00;
	ret = cxd6866_write_regs(tuner, 0x5e, data, 3);
	if (ret)
		return ret;

	/* Power save setting for analog block (0x0C):
	 * SAT_NORMAL, terrRfActive=0 -> 0x14 */
	ret = cxd6866_write_reg(tuner, 0x0c, 0x14);
	if (ret)
		return ret;

	/* 0x79, 0x7A, 0x7B */
	data[0] = 0x9e;
	data[1] = 0x00;
	data[2] = 0x00;
	ret = cxd6866_write_regs(tuner, 0x79, data, 3);
	if (ret)
		return ret;

	/* 0x99, 0x9A, 0x9B: XOSC setting / OVLD */
	data[0] = 0xa9;
	data[1] = 0x01;
	data[2] = 0x00;
	ret = cxd6866_write_regs(tuner, 0x99, data, 3);
	if (ret)
		return ret;

	/* 0x81 - 0x94 */
	/* Frequency setting for crystal oscillator (0x81) */
	switch (tuner->config.xtal) {
	case 16000:
		data[0] = 0x10;
		break;
	case 24000:
		data[0] = 0x18;
		break;
	default:
		return -EINVAL;
	}
	/* Driver current / load capacitance for XOSC (0x82, 0x83):
	 * XOSC_APC_EN=1, XOSC_SEL=0x04 (100 uA); XOSC_CALC_EN=1,
	 * XOSC_CAP_SET=0x30 (12 pF).  XOSC_CAP_SET follows the value the
	 * XIT-SQR100 Windows driver (IT9300BDA.sys) writes; the Sony
	 * reference driver uses 0x1E (7.5 pF, for a 6 pF crystal). */
	data[1] = (u8)(0x80 | 0x04);
	data[2] = (u8)(0x80 | 0x30);
	/* REFOUT disabled (0x84) */
	data[3] = 0x00;
	/* GPIO0 / GPIO1 port setting (0x85, 0x86) */
	data[4] = 0x00;
	data[5] = 0x00;
	/* Clock enable for internal logic block (0x87) */
	data[6] = 0x84;
	/* Start CPU boot-up (0x88) */
	data[7] = 0x40;
	/* For burst-write (0x89) */
	data[8] = 0x10;
	/* Setting for internal RFAGC (0x8A, 0x8B, 0x8C) */
	data[9] = 0x00;
	data[10] = 0x45;
	data[11] = 0x75;
	/* Setting for analog block (0x8D) */
	data[12] = 0x01;
	/* Initial setting for internal analog block (0x8E..0x94) */
	data[13] = 0x00;
	data[14] = 0x00;
	data[15] = 0x00;
	data[16] = 0x0a;
	data[17] = 0x0c;
	data[18] = 0x3f;
	data[19] = 0x00;
	ret = cxd6866_write_regs(tuner, 0x81, data, 20);
	if (ret)
		return ret;

	/* Local block filter setting (0x22, 0x23) */
	data[0] = 0x00;
	data[1] = 0x08;
	ret = cxd6866_write_regs(tuner, 0x22, data, 2);
	if (ret)
		return ret;

	/* Initial setting for RF (0x46) */
	ret = cxd6866_write_reg(tuner, 0x46, 0x00);
	if (ret)
		return ret;

	msleep(10);

	/* Check CPU_STT (0x1A): must be 0x00 */
	ret = cxd6866_read_reg(tuner, 0x1a, &tmp);
	if (ret)
		return ret;
	if (tmp != 0x00) {
		dev_err(tuner->dev, "cxd6866: CPU_STT(0x1A)=0x%02x (expected 0x00)\n",
			tmp);
		return -EIO;
	}

	/* SRAM status check (0x17, 0x18) */
	data[0] = 0x7f;
	data[1] = 0x06;
	ret = cxd6866_write_regs(tuner, 0x17, data, 2);
	if (ret)
		return ret;

	msleep(1);

	/* Filter setting: read 0x19, select MIX_GAIN (0x79) */
	ret = cxd6866_read_reg(tuner, 0x19, &tmp);
	if (ret)
		return ret;
	ret = cxd6866_write_reg(tuner, 0x79, (tmp == 0x00) ? 0x00 : 0x9e);
	if (ret)
		return ret;

	/* Disable IF signal output (IF_OUT_SEL) (0x74) */
	ret = cxd6866_write_reg(tuner, 0x74, 0x02);
	if (ret)
		return ret;

	/* IFOUT DC bias setting (0xA0): 750 mV default -> 0x00 */
	ret = cxd6866_write_reg(tuner, 0xa0, 0x00);
	if (ret)
		return ret;

	/* Standby setting for CPU (0x88) */
	ret = cxd6866_write_reg(tuner, 0x88, 0x00);
	if (ret)
		return ret;

	/* Standby setting for internal logic block (0x87) */
	ret = cxd6866_write_reg(tuner, 0x87, 0x80);
	if (ret)
		return ret;

	/* Load capacitance control setting for crystal oscillator (0x80) */
	ret = cxd6866_write_reg(tuner, 0x80, 0x01);
	if (ret)
		return ret;

	/* Satellite initial setting (0x41, 0x42) */
	data[0] = 0x00;
	data[1] = 0x00;
	ret = cxd6866_write_regs(tuner, 0x41, data, 2);
	if (ret)
		return ret;

	/* Satellite initial setting (0x45..0x4A, 0xA6, 0xA7):
	 * no SAT_LOW_GAIN_MODE -> data[3]=0x11 */
	data[0] = 0x0a;
	data[1] = 0x00;
	data[2] = 0x00;
	data[3] = 0x11;
	data[4] = 0x00;
	data[5] = 0x03;
	ret = cxd6866_write_regs(tuner, 0x45, data, 6);
	if (ret)
		return ret;
	data[0] = 0x66;
	data[1] = 0x08;
	ret = cxd6866_write_regs(tuner, 0xa6, data, 2);
	if (ret)
		return ret;

	/* PFD disable for XOSC issue for Multi tuner usage (0x1E) */
	ret = cxd6866_write_reg(tuner, 0x1e, 0xa0);
	if (ret)
		return ret;

	return 0;
}

/*
 * TER_tune: terrestrial (ISDB-T) tuning, with vcoCal=1 (calibration pass).
 * Parameters are for SONY_FREIA_DTV_ISDBT_6 (ISDB-T 6 MHz BW):
 *   RF_GAIN=AUTO, IF_BPF_GC=0x06, RFOVLD_DET_LV1=0x0E (all bands),
 *   IFOVLD_DET_LV=0x03 (all bands), IF_BPF_F0=0x00, BW=6MHz(0x00),
 *   FIF_OFFSET=OFFSET(-9)=0x17, BW_OFFSET=OFFSET(-5)=0x1B.
 * The PLL loop filter (0x61-0x66, 0x9C-0x9D) and IFOUT_LIMIT (0x68) are
 * selected by config.loop_filter_internal / config.outlmt_dtv_1_2vpp.
 */
static int cxd6866_ter_tune(struct cxd6866_tuner *tuner, u32 freq_khz)
{
	int ret;
	u8 data[17];
	u8 data6[6];
	u8 data2[3];

	/* Mode select (0x01): terrestrial */
	ret = cxd6866_write_reg(tuner, 0x01, 0x00);
	if (ret)
		return ret;

	/* vcoCal: disable IF signal output (0x74) */
	ret = cxd6866_write_reg(tuner, 0x74, 0x02);
	if (ret)
		return ret;

	/* Clock enable for internal logic block, CPU wake-up (0x87, 0x88) */
	data2[0] = 0x84;
	data2[1] = 0x40;
	ret = cxd6866_write_regs(tuner, 0x87, data2, 2);
	if (ret)
		return ret;

	/* OVLD block (0x3C, 0x3D, 0x3E, 0x52, 0x8B):
	 * non-cable, non-external-OVLD-TC */
	data2[0] = 0xfc;
	data2[1] = 0x9c;
	data2[2] = 0x8f;
	ret = cxd6866_write_regs(tuner, 0x3c, data2, 3);
	if (ret)
		return ret;
	ret = cxd6866_write_reg(tuner, 0x52, 0x00);
	if (ret)
		return ret;
	ret = cxd6866_write_reg(tuner, 0x8b, 0x75);
	if (ret)
		return ret;

	/* Setting for internal analog block (0x8D) */
	ret = cxd6866_write_reg(tuner, 0x8d, 0x01);
	if (ret)
		return ret;

	/* Initial setting for internal analog block (0x91, 0x92):
	 * ISDB-T is not DVB-T/T2 -> {0x0A, 0x0C} */
	data2[0] = 0x0a;
	data2[1] = 0x0c;
	ret = cxd6866_write_regs(tuner, 0x91, data2, 2);
	if (ret)
		return ret;

	/* PLL block (0x9C, 0x9D), non-ATV */
	if (tuner->config.loop_filter_internal) {
		data2[0] = 0x8c;
		data2[1] = 0x01;
	} else {
		data2[0] = 0x05;
		data2[1] = 0x00;
	}
	ret = cxd6866_write_regs(tuner, 0x9c, data2, 2);
	if (ret)
		return ret;

	/* Enable for analog block (0x5E, 0x5F, 0x60) */
	ret = cxd6866_write_reg(tuner, 0x7c, 0x01);
	if (ret)
		return ret;
	data2[0] = 0x6e;
	data2[1] = 0x02;
	data2[2] = 0x9e;
	ret = cxd6866_write_regs(tuner, 0x5e, data2, 3);
	if (ret)
		return ret;
	ret = cxd6866_write_reg(tuner, 0x7c, 0x00);
	if (ret)
		return ret;
	ret = cxd6866_write_reg(tuner, 0x1e, 0xa4);
	if (ret)
		return ret;
	ret = cxd6866_write_reg(tuner, 0x5e, 0xee);
	if (ret)
		return ret;

	/* 0x61 - 0x66: vcoCal, non-ATV */
	data6[0] = 0x66; /* 0x61: vcoCal */
	data6[1] = 0x01; /* 0x62: REF_R */
	if (tuner->config.loop_filter_internal) {
		data6[2] = 0x38; /* 0x63 */
		data6[3] = 0x1e; /* 0x64 */
		data6[4] = 0x02; /* 0x65 */
		data6[5] = 0x24; /* 0x66 */
	} else {
		data6[2] = 0x5a; /* 0x63 */
		data6[3] = 0x78; /* 0x64 */
		data6[4] = 0x08; /* 0x65 */
		data6[5] = 0x32; /* 0x66 */
	}
	ret = cxd6866_write_regs(tuner, 0x61, data6, 6);
	if (ret)
		return ret;

	/* LT_AMP_EN should be 0 (0x67 bit 1) */
	ret = cxd6866_write_reg_mask(tuner, 0x67, 0x00, 0x02);
	if (ret)
		return ret;

	/* 0x68 - 0x78 */
	/* IFOUT_LIMIT (0x68): DTV 1.2 Vpp if OUTLMT_DTV_1_2Vpp set */
	data[0] = (u8)(tuner->config.outlmt_dtv_1_2vpp ? 0x01 : 0x00);
	/* RF_GAIN=AUTO -> 0x80; IF_BPF_GC=0x06 -> 0x86 (0x69) */
	data[1] = 0x80 | 0x06;
	/* RFAGC normal (0x6A) */
	data[2] = 0x00;
	/* RFOVLD_DET_LV1 (0x6B) = 0x0E; IFOVLD_DET_LV (0x6C) = 0x03 | 0x30 */
	data[3] = 0x0e;
	data[4] = 0x03 | 0x30;
	/* IF_BPF_F0 (0x6D bits 5:4)=0, BW (bits 1:0)=6MHz(0x00) -> 0x00 */
	data[5] = 0x00;
	/* FIF_OFFSET (0x6E) = OFFSET(-9) = 0x17 */
	data[6] = 0x17;
	/* BW_OFFSET (0x6F) = OFFSET(-5) = 0x1B */
	data[7] = 0x1b;
	/* RF tuning frequency (0x70, 0x71, 0x72) */
	data[8] = (u8)(freq_khz & 0xff);
	data[9] = (u8)((freq_khz >> 8) & 0xff);
	data[10] = (u8)((freq_khz >> 16) & 0x1f);
	/* Tuning command (0x73): vcoCal -> 0xFF */
	data[11] = 0xff;
	/* IFOUT / AGC pin selection (0x74): CXD6866AER -> 0x00 */
	data[12] = 0x00;
	/* Analog block tuning (0x75..0x78) */
	data[13] = 0xf1;
	data[14] = 0x0f;
	data[15] = 0x06;
	data[16] = 0x03;
	ret = cxd6866_write_regs(tuner, 0x68, data, 17);
	if (ret)
		return ret;

	return 0;
}

/*
 * TER_tune_end: completes the terrestrial tuning sequence.
 */
static int cxd6866_ter_tune_end(struct cxd6866_tuner *tuner)
{
	int ret;

	/* Standby setting for CPU (0x88) */
	ret = cxd6866_write_reg(tuner, 0x88, 0x00);
	if (ret)
		return ret;

	/* Standby setting for internal logic block (0x87) */
	ret = cxd6866_write_reg(tuner, 0x87, 0x80);
	if (ret)
		return ret;

	return 0;
}

/*
 * SAT_tune: satellite (ISDB-S) tuning, with vcoCal=1 (calibration pass).
 * The PLL loop filter (0x04-0x0B, 0x44) is selected by
 * config.loop_filter_internal; no SAT_LOW_GAIN_MODE; ISDB-S LPF = 22 MHz.
 */
static int cxd6866_sat_tune(struct cxd6866_tuner *tuner, u32 freq_khz)
{
	int ret;
	u8 data[8];
	u8 data7[7];
	u32 freq4_khz;

	/* vcoCal: disable IF signal output (0x15) */
	ret = cxd6866_write_reg(tuner, 0x15, 0x02);
	if (ret)
		return ret;

	/* RFIN matching in power save (SAT) reset (0x43) */
	ret = cxd6866_write_reg(tuner, 0x43, 0xc0);
	if (ret)
		return ret;

	/* Satellite mode select (0x01) */
	ret = cxd6866_write_reg(tuner, 0x01, 0x01);
	if (ret)
		return ret;

	/* Reset analog block setting (0x6B) */
	ret = cxd6866_write_reg(tuner, 0x6b, 0x00);
	if (ret)
		return ret;

	/* Tuning setting for CPU (0x40): vcoCal -> 0x06 */
	ret = cxd6866_write_reg(tuner, 0x40, 0x06);
	if (ret)
		return ret;

	/* 0x04 - 0x0B: clock enable + PLL */
	data[0] = 0x84; /* 0x04 */
	data[1] = 0x40; /* 0x05 */
	data[2] = 0x01; /* 0x06: REF_R */
	if (tuner->config.loop_filter_internal) {
		data[3] = 0x8a; /* 0x07 */
		data[4] = 0x38; /* 0x08 */
		data[5] = 0x1e; /* 0x09 */
		data[6] = 0x02; /* 0x0A */
		data[7] = 0x24; /* 0x0B */
	} else {
		data[3] = 0x05; /* 0x07 */
		data[4] = 0x5a; /* 0x08 */
		data[5] = 0x78; /* 0x09 */
		data[6] = 0x08; /* 0x0A */
		data[7] = 0x31; /* 0x0B */
	}
	ret = cxd6866_write_regs(tuner, 0x04, data, 8);
	if (ret)
		return ret;

	/* 0x44: internal loop if LOOPFILTER_INTERNAL */
	ret = cxd6866_write_reg(tuner, 0x44,
				(u8)(tuner->config.loop_filter_internal ? 0x01 : 0x00));
	if (ret)
		return ret;

	/* Enable for analog block (0x0C, 0x0D, 0x0E):
	 * no SAT_LOW_GAIN_MODE -> 0x0F; POWERSAVE_TERR normal -> |0x70 -> 0x7F */
	ret = cxd6866_write_reg(tuner, 0x7c, 0x01);
	if (ret)
		return ret;
	data[0] = 0x7f;
	data[1] = 0x02;
	data[2] = 0x9e;
	ret = cxd6866_write_regs(tuner, 0x0c, data, 3);
	if (ret)
		return ret;
	ret = cxd6866_write_reg(tuner, 0x7c, 0x00);
	if (ret)
		return ret;
	ret = cxd6866_write_reg(tuner, 0x1e, 0xa4);
	if (ret)
		return ret;
	/* second 0x0C write: 0x0F | 0xF0 = 0xFF */
	ret = cxd6866_write_reg(tuner, 0x0c, 0xff);
	if (ret)
		return ret;

	/* 0x0F - 0x15: LPF + RF frequency + tuning command + IQ output */
	freq4_khz = (freq_khz + 2) / 4;
	data7[0] = 22; /* 0x0F: LPF cutoff 22 MHz (ISDB-S) */
	data7[1] = (u8)(freq4_khz & 0xff);      /* 0x10: FRF_L */
	data7[2] = (u8)((freq4_khz >> 8) & 0xff); /* 0x11: FRF_M */
	data7[3] = (u8)((freq4_khz >> 16) & 0x1f); /* 0x12: FRF_H */
	data7[4] = 0xff; /* 0x13: tuning command, vcoCal -> 0xFF */
	data7[5] = 0x00; /* 0x14: IQOUT_LIMIT (no STV_0_6Vpp) */
	data7[6] = 0x01; /* 0x15: enable IQ output */
	ret = cxd6866_write_regs(tuner, 0x0f, data7, 7);
	if (ret)
		return ret;

	return 0;
}

/*
 * SAT_tune_end: completes the satellite tuning sequence.
 */
static int cxd6866_sat_tune_end(struct cxd6866_tuner *tuner)
{
	int ret;

	/* Standby setting for CPU (0x05) */
	ret = cxd6866_write_reg(tuner, 0x05, 0x00);
	if (ret)
		return ret;

	/* Standby setting for internal logic block (0x04) */
	ret = cxd6866_write_reg(tuner, 0x04, 0x80);
	if (ret)
		return ret;

	return 0;
}

/*
 * TER_fin: power down from terrestrial mode.
 * POWERSAVE_TERR = NORMAL_MATCHING_DISABLE -> data={0x15,0x00,0x00,0x00}
 */
static int cxd6866_ter_fin(struct cxd6866_tuner *tuner)
{
	int ret;
	u8 data[3];

	/* Disable IF signal output (0x74) */
	ret = cxd6866_write_reg(tuner, 0x74, 0x02);
	if (ret)
		return ret;

	/* Keep RF_EXT bit, set power save (0x67) */
	ret = cxd6866_write_reg_mask(tuner, 0x67, 0x00, 0xfe);
	if (ret)
		return ret;

	/* Power save analog block (0x5E) */
	ret = cxd6866_write_reg(tuner, 0x5e, 0x6e);
	if (ret)
		return ret;
	ret = cxd6866_write_reg(tuner, 0x1e, 0xa0);
	if (ret)
		return ret;
	data[0] = 0x15;
	data[1] = 0x00;
	data[2] = 0x00;
	ret = cxd6866_write_regs(tuner, 0x5e, data, 3);
	if (ret)
		return ret;

	/* Reset OVLD block (0x3D, 0x3E) */
	data[0] = 0x00;
	data[1] = 0x00;
	ret = cxd6866_write_regs(tuner, 0x3d, data, 2);
	if (ret)
		return ret;

	/* Standby setting for CPU (0x88) */
	ret = cxd6866_write_reg(tuner, 0x88, 0x00);
	if (ret)
		return ret;

	/* Standby setting for internal logic block (0x87) */
	ret = cxd6866_write_reg(tuner, 0x87, 0x80);
	if (ret)
		return ret;

	return 0;
}

/*
 * SAT_fin: power down from satellite mode.
 * POWERSAVE_SAT = SAT_NORMAL, terrRfActive=0 -> data={0x14,0x00,0x00,0xC0}
 */
static int cxd6866_sat_fin(struct cxd6866_tuner *tuner)
{
	int ret;
	u8 data[3];

	/* Disable IQ signal output (0x15) */
	ret = cxd6866_write_reg(tuner, 0x15, 0x02);
	if (ret)
		return ret;

	/* RFIN matching in power save (SAT) (0x43) */
	ret = cxd6866_write_reg(tuner, 0x43, 0xc0);
	if (ret)
		return ret;

	/* no SAT_LOW_GAIN_MODE -> 0x0C = 0x7F */
	ret = cxd6866_write_reg(tuner, 0x0c, 0x7f);
	if (ret)
		return ret;
	ret = cxd6866_write_reg(tuner, 0x1e, 0xa0);
	if (ret)
		return ret;
	data[0] = 0x14;
	data[1] = 0x00;
	data[2] = 0x00;
	ret = cxd6866_write_regs(tuner, 0x0c, data, 3);
	if (ret)
		return ret;

	/* Return to terrestrial mode (0x01) */
	ret = cxd6866_write_reg(tuner, 0x01, 0x00);
	if (ret)
		return ret;

	/* Standby setting for CPU (0x05) */
	ret = cxd6866_write_reg(tuner, 0x05, 0x00);
	if (ret)
		return ret;

	/* Standby setting for internal logic block (0x04) */
	ret = cxd6866_write_reg(tuner, 0x04, 0x80);
	if (ret)
		return ret;

	return 0;
}

int cxd6866_init(struct cxd6866_tuner *tuner)
{
	int ret;

	if (!tuner || !tuner->dev || !tuner->i2c || !tuner->i2c_addr)
		return -EINVAL;
	if (tuner->config.xtal != 16000 && tuner->config.xtal != 24000)
		return -EINVAL;

	tuner->system = CXD6866_UNSPECIFIED_SYSTEM;

	ret = i2c_comm_master_gate_ctrl(tuner->i2c, true);
	if (ret)
		return ret;

	/* Detect chip and run the power-on sequence */
	ret = cxd6866_detect(tuner);
	if (!ret)
		ret = cxd6866_x_pon(tuner);

	i2c_comm_master_gate_ctrl(tuner->i2c, false);

	if (ret)
		dev_err(tuner->dev, "cxd6866_init: failed (%d)\n", ret);

	return ret;
}

int cxd6866_term(struct cxd6866_tuner *tuner)
{
	int ret;
	int gate_ret;

	if (!tuner || !tuner->i2c)
		return -EINVAL;

	if (tuner->system == CXD6866_UNSPECIFIED_SYSTEM)
		return 0;

	ret = i2c_comm_master_gate_ctrl(tuner->i2c, true);
	if (ret)
		return ret;

	switch (tuner->system) {
	case CXD6866_ISDB_T_SYSTEM:
		ret = cxd6866_ter_fin(tuner);
		break;
	case CXD6866_ISDB_S_SYSTEM:
		ret = cxd6866_sat_fin(tuner);
		break;
	default:
		ret = 0;
		break;
	}

	/* A failed power-down leaves the hardware state unknown. */
	tuner->system = CXD6866_UNSPECIFIED_SYSTEM;

	gate_ret = i2c_comm_master_gate_ctrl(tuner->i2c, false);
	return ret ? ret : gate_ret;
}

int cxd6866_set_params_t(struct cxd6866_tuner *tuner,
			 enum cxd6866_system system,
			 u32 freq, u32 bandwidth)
{
	int ret;

	if (!tuner || !tuner->i2c)
		return -EINVAL;
	if (system != CXD6866_ISDB_T_SYSTEM)
		return -EINVAL;
	if (bandwidth != 6)
		return -EINVAL;
	if (freq < 1000 || freq > 1200000)
		return -ERANGE;

	ret = i2c_comm_master_gate_ctrl(tuner->i2c, true);
	if (ret)
		return ret;

	/* If currently in satellite mode, power it down first */
	if (tuner->system == CXD6866_ISDB_S_SYSTEM) {
		ret = cxd6866_sat_fin(tuner);
		if (ret)
			goto exit;
		tuner->system = CXD6866_UNSPECIFIED_SYSTEM;
	}

	/* TER_tune (vcoCal) then wait 50 ms and TER_tune_end */
	ret = cxd6866_ter_tune(tuner, freq);
	if (ret)
		goto exit;

	msleep(50);

	ret = cxd6866_ter_tune_end(tuner);
	if (ret)
		goto exit;

	tuner->system = CXD6866_ISDB_T_SYSTEM;

exit:
	if (ret)
		tuner->system = CXD6866_UNSPECIFIED_SYSTEM;
	{
		int gate_ret = i2c_comm_master_gate_ctrl(tuner->i2c, false);

		if (!ret)
			ret = gate_ret;
	}
	return ret;
}

int cxd6866_set_params_s(struct cxd6866_tuner *tuner,
			 enum cxd6866_system system,
			 u32 freq, u32 symbol_rate)
{
	int ret;

	if (!tuner || !tuner->i2c)
		return -EINVAL;
	if (system != CXD6866_ISDB_S_SYSTEM)
		return -EINVAL;
	if (freq < 500000 || freq > 3500000)
		return -ERANGE;

	/* ISDB-S uses a fixed symbol rate; the caller value is not configurable. */
	(void)symbol_rate;

	ret = i2c_comm_master_gate_ctrl(tuner->i2c, true);
	if (ret)
		return ret;

	/* If currently in terrestrial mode, power it down first */
	if (tuner->system == CXD6866_ISDB_T_SYSTEM) {
		ret = cxd6866_ter_fin(tuner);
		if (ret)
			goto exit;
		tuner->system = CXD6866_UNSPECIFIED_SYSTEM;
	}

	/* SAT_tune (vcoCal), wait 10 ms, then SAT_tune_end */
	ret = cxd6866_sat_tune(tuner, freq);
	if (ret)
		goto exit;

	msleep(10);

	ret = cxd6866_sat_tune_end(tuner);
	if (ret)
		goto exit;

	/* Match sony_tuner_freia_SatTune's post-tune stabilization wait. */
	msleep(50);

	tuner->system = CXD6866_ISDB_S_SYSTEM;

exit:
	if (ret)
		tuner->system = CXD6866_UNSPECIFIED_SYSTEM;
	{
		int gate_ret = i2c_comm_master_gate_ctrl(tuner->i2c, false);

		if (!ret)
			ret = gate_ret;
	}
	return ret;
}

int cxd6866_stop(struct cxd6866_tuner *tuner)
{
	int ret = 0;
	int gate_ret;

	if (!tuner || !tuner->i2c)
		return -EINVAL;

	if (tuner->system == CXD6866_UNSPECIFIED_SYSTEM)
		return -EALREADY;

	ret = i2c_comm_master_gate_ctrl(tuner->i2c, true);
	if (ret)
		return ret;

	switch (tuner->system) {
	case CXD6866_ISDB_T_SYSTEM:
		ret = cxd6866_ter_fin(tuner);
		break;
	case CXD6866_ISDB_S_SYSTEM:
		ret = cxd6866_sat_fin(tuner);
		break;
	default:
		break;
	}

	/* A failed stop leaves the hardware state unknown, so force re-init. */
	tuner->system = CXD6866_UNSPECIFIED_SYSTEM;
	gate_ret = i2c_comm_master_gate_ctrl(tuner->i2c, false);
	return ret ? ret : gate_ret;
}
