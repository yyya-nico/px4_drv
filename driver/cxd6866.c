// SPDX-License-Identifier: GPL-2.0-only
/*
 * Sony CXD6866AER tuner driver (cxd6866.c)
 *
 * NOTE: The CXD6866AER is the satellite/terrestrial tuner LSI used in the
 * XIT-SQR100 (USB VID 0x06B8, PID 0x106B).  This driver mirrors the
 * structure of the sibling Sony tuner driver (cxd2858er.c) so that device
 * code can treat both tuners uniformly.
 *
 * IMPORTANT / NOT YET FUNCTIONAL:
 *   The register map, I2C address and crystal frequency of the
 *   CXD6866AER as used in the XIT-SQR100 have not been confirmed against
 *   a datasheet or real hardware.  The tuning entry points below therefore
 *   return -EOPNOTSUPP instead of issuing I2C transactions, so that a
 *   mis-configured tuner is never presented to the user as "tuned".
 *   The I2C helper primitives are provided and compiled, but they must be
 *   replaced with the verified register sequences (and the -EOPNOTSUPP
 *   returns removed) before this driver is considered functional.
 */

#include "print_format.h"
#include "cxd6866.h"

#ifdef __linux__
#include <linux/delay.h>
#endif

/*
 * The I2C helper primitives below are provided as a starting point for the
 * verified tuning sequences.  They are intentionally not yet referenced from
 * the public entry points, so they are marked __maybe_unused to keep the
 * -Werror build clean until the register sequences are implemented.
 */
static int __maybe_unused cxd6866_read_regs(struct cxd6866_tuner *tuner,
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

static int __maybe_unused cxd6866_read_reg(struct cxd6866_tuner *tuner,
					   u8 reg, u8 *val)
{
	return cxd6866_read_regs(tuner, reg, val, 1);
}

static int __maybe_unused cxd6866_write_regs(struct cxd6866_tuner *tuner,
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

static int __maybe_unused cxd6866_write_reg(struct cxd6866_tuner *tuner,
					    u8 reg, u8 val)
{
	return cxd6866_write_regs(tuner, reg, &val, 1);
}

static int __maybe_unused cxd6866_write_reg_mask(struct cxd6866_tuner *tuner,
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
 * NOTE: Unused helpers are referenced via the public API below so that the
 * compiler does not discard them; they are provided as a starting point for
 * the verified tuning sequences.
 */
void cxd6866_term(struct cxd6866_tuner *tuner)
{
	/*
	 * TODO: Once the verified register map is known, issue a stop /
	 * power-down sequence here, mirroring cxd2858er_stop().
	 *
	 * For now there is nothing to undo because init() does not issue
	 * any I2C traffic either.
	 */
	(void)tuner;
}

int cxd6866_init(struct cxd6866_tuner *tuner)
{
	if (!tuner || !tuner->i2c)
		return -EINVAL;

	/*
	 * TODO: Populate and verify the power-on / initialization sequence
	 * for the CXD6866AER (register map, LNA enable, crystal
	 * configuration) before enabling I2C traffic.  Until then, report
	 * that the tuner is not yet supported rather than issuing
	 * unverified transactions.
	 */
	dev_warn(tuner->dev,
		 "cxd6866: tuner init not yet implemented (register map unverified)\n");

	return -EOPNOTSUPP;
}

int cxd6866_set_params_t(struct cxd6866_tuner *tuner,
			 enum cxd6866_system system,
			 u32 freq, u32 bandwidth)
{
	(void)system;
	(void)freq;
	(void)bandwidth;

	if (!tuner || !tuner->i2c)
		return -EINVAL;

	/*
	 * TODO: Implement ISDB-T (terrestrial) tuning once the verified
	 * register sequence is available.  Mirrors
	 * cxd2858er_set_params_t().
	 */
	return -EOPNOTSUPP;
}

int cxd6866_set_params_s(struct cxd6866_tuner *tuner,
			 enum cxd6866_system system,
			 u32 freq, u32 symbol_rate)
{
	(void)system;
	(void)freq;
	(void)symbol_rate;

	if (!tuner || !tuner->i2c)
		return -EINVAL;

	/*
	 * TODO: Implement ISDB-S (satellite) tuning once the verified
	 * register sequence is available.  Mirrors
	 * cxd2858er_set_params_s().
	 */
	return -EOPNOTSUPP;
}

int cxd6866_stop(struct cxd6866_tuner *tuner)
{
	(void)tuner;

	/*
	 * TODO: Implement the stop sequence once the verified register map
	 * is available.  Mirrors cxd2858er_stop().
	 */
	return 0;
}
