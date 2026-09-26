// SPDX-License-Identifier: GPL-2.0-only
/*
 * Sony CXD6866AER tuner driver definitions (cxd6866.h)
 *
 * NOTE: The CXD6866AER is a satellite/terrestrial tuner LSI used in the
 * XIT-SQR100.  This header mirrors the public API shape of the sibling
 * Sony tuner driver (cxd2858er.h) so that device code can treat both
 * tuners uniformly.
 *
 * WARNING: The register map, I2C address and crystal frequency used with
 * the CXD6866AER in the XIT-SQR100 have NOT been confirmed against a
 * datasheet or real hardware.  The implementation in cxd6866.c is a
 * placeholder that compiles and returns a well-defined "not yet supported"
 * result; the values MUST be verified on the target board before this
 * driver is considered functional.  See the TODO markers in cxd6866.c.
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
	u32 xtal;
	struct {
		bool lna;
	} ter;
	struct {
		bool lna;
	} sat;
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
