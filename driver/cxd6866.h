// SPDX-License-Identifier: GPL-2.0-only
/*
 * Sony CXD6866AER (FREIA) tuner driver definitions (cxd6866.h)
 *
 * The CXD6866AER is a terrestrial/satellite tuner LSI used in the
 * XIT-SQR100.  The sequences in cxd6866.c follow the Sony FREIA
 * reference driver (docs/terr_cable_sat_freia/refcode/sony_freia.c,
 * "Based on FREIA application note 1.2.0"), which is the same silicon
 * the XIT-SQR100 Windows driver (IT9300BDA.sys) drives through the
 * sony_cxd2856_tuner_freia_* API.
 *
 * Board settings taken from IT9300BDA.sys (see
 * docs/20260926_xitsqr100_implementation_summary.md):
 *   - sony_cxd2856_tuner_freia_Create() is called with config flags
 *     0x10004000 = SONY_FREIA_CONFIG_LOOPFILTER_INTERNAL
 *                 | SONY_FREIA_CONFIG_OUTLMT_DTV_1_2Vpp
 *   - tuner I2C 7-bit address 0x60 (the driver passes the 8-bit form 0xC0)
 *
 * The XIT-SQR100 board settings taken from IT9300BDA.sys:
 *   - 24 MHz crystal (register 0x81 = 0x18 in the binary's X_pon)
 *   - XOSC_SEL = 0x04 (100 uA), XOSC_CAP_SET = 0x30 (12 pF)
 *   - LOOPFILTER_INTERNAL + OUTLMT_DTV_1_2Vpp (config flags 0x10004000)
 *   - tuner I2C 7-bit address 0x60 (the driver passes the 8-bit form 0xC0)
 *
 * The FREIA part has no standalone LNA enable bit; the RFIN/LNA state is
 * selected through the power-save mode (SAT_NORMAL /
 * NORMAL_MATCHING_DISABLE), so there is no per-band LNA flag here.
 *
 * Still not verifiable from the Windows driver: IT9303 board power GPIO pins
 * and polarity (they are outside the tuner I2C register map).
 */

#ifndef __CXD6866_H__
#define __CXD6866_H__

#ifdef __linux__
#include <linux/types.h>
#include <linux/device.h>
#elif defined(_WIN32) || defined(_WIN64)
#include "misc_win.h"
#endif

#include "i2c_comm.h"

struct cxd6866_config {
	/* Crystal frequency in kHz. 16000 -> 0x10, 24000 -> 0x18 (register
	 * 0x81).  The XIT-SQR100 uses a 24 MHz crystal (confirmed in
	 * IT9300BDA.sys). */
	u32 xtal;
	/*
	 * SONY_FREIA_CONFIG_LOOPFILTER_INTERNAL (0x10000000): use the
	 * internal PLL loop filter.  Set by the XIT-SQR100 Windows driver.
	 */
	bool loop_filter_internal;
	/*
	 * SONY_FREIA_CONFIG_OUTLMT_DTV_1_2Vpp (0x00004000): limit the
	 * digital IF output amplitude to 1.2 Vpp.  Set by the XIT-SQR100
	 * Windows driver.
	 */
	bool outlmt_dtv_1_2vpp;
};

enum cxd6866_system {
	CXD6866_UNSPECIFIED_SYSTEM = 0,
	CXD6866_ISDB_T_SYSTEM,
	CXD6866_ISDB_S_SYSTEM
};

struct cxd6866_tuner {
	const struct device *dev;
	const struct i2c_comm_master *i2c;
	u8 i2c_addr;
	struct cxd6866_config config;
	enum cxd6866_system system;
};

#ifdef __cplusplus
extern "C" {
#endif
int cxd6866_init(struct cxd6866_tuner *tuner);
int cxd6866_term(struct cxd6866_tuner *tuner);

int cxd6866_set_params_t(struct cxd6866_tuner *tuner,
			 enum cxd6866_system system,
			 u32 freq, u32 bandwidth);
int cxd6866_set_params_s(struct cxd6866_tuner *tuner,
			 enum cxd6866_system system,
			 u32 freq, u32 symbol_rate);
int cxd6866_stop(struct cxd6866_tuner *tuner);
#ifdef __cplusplus
}
#endif

#endif
