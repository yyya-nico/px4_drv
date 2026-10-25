// SPDX-License-Identifier: GPL-2.0-only
/*
 * 実ソースを模擬 I2C で動かし、XIT-SQR100 の実機キャプチャと
 * PXMLT の従来設定を照合する。カーネル・Windows SDK は不要。
 * cc -std=c11 -Wall -Wextra driver/tests/cxd2856er_ts_clock_test.c -o /tmp/cxd2856er_ts_clock_test
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

/* この翻訳単位ではハードウェア待機と OS 型だけを置き換える。 */
#undef __linux__
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
static void msleep(unsigned int ms) { (void)ms; }
#include "../cxd2856er.c"

struct mock_i2c {
	u8 regs[256][256];
	u8 bank;
	u8 fail_reg;
	bool fail_enabled;
	unsigned int clock_enable_writes;
};

static int request(void *priv, const struct i2c_comm_request *req, int num)
{
	struct mock_i2c *mock = priv;
	u8 reg = req[0].data[0];
	int i;

	assert(req[0].addr == 0x64);
	assert(req[0].req == I2C_WRITE_REQUEST);
	if (mock->fail_enabled && reg == mock->fail_reg)
		return -EIO;
	if (num == 2) {
		assert(req[0].len == 1 && req[1].req == I2C_READ_REQUEST);
		assert(req[1].addr == 0x64);
		memcpy(req[1].data, &mock->regs[mock->bank][reg], req[1].len);
		return 0;
	}
	assert(num == 1 && req[0].len >= 2);
	if (!reg) {
		assert(req[0].len == 2);
		mock->bank = req[0].data[1];
		return 0;
	}
	for (i = 1; i < req[0].len; i++) {
		mock->regs[mock->bank][reg + i - 1] = req[0].data[i];
		if (!mock->bank && reg == 0x32 && (req[0].data[i] & 1))
			mock->clock_enable_writes++;
	}
	return 0;
}

static void check_clock(bool serial, enum cxd2856er_system system,
			u8 mode, u8 duty, u8 period, u8 source)
{
	struct mock_i2c mock = {0};
	struct i2c_comm_master master = { .request = request, .priv = &mock };
	struct cxd2856er_demod demod = {
		.i2c = &master, .i2c_addr.slvt = 0x64,
		.config.serial_ts_clock = serial
	};

	/* clock の設定以外のビットを保持することも確認する。 */
	mock.regs[0][0xc4] = 0xac;
	mock.regs[0][0xd1] = 0x80;
	mock.regs[0][0x33] = 0xc0;
	assert(cxd2856er_set_ts_clock(&demod, system) == 0);
	assert(mock.regs[0][0xc4] == (0xac | mode));
	assert(mock.regs[0][0xd1] == (0x80 | duty));
	assert(mock.regs[0][0xd9] == period);
	assert(mock.regs[0][0x33] == (0xc0 | source));
	assert(mock.clock_enable_writes == 1);
}

int main(void)
{
	struct mock_i2c mock = { .fail_enabled = true, .fail_reg = 0x33 };
	struct i2c_comm_master master = { .request = request, .priv = &mock };
	struct cxd2856er_demod demod = {
		.i2c = &master, .i2c_addr.slvt = 0x64,
		.config.serial_ts_clock = true
	};

	/* XIT-SQR100: Windows キャプチャの clock mode / duty / period / source。 */
	check_clock(true, CXD2856ER_ISDB_T_SYSTEM, 2, 2, 16, 0);
	check_clock(true, CXD2856ER_ISDB_S_SYSTEM, 1, 1, 8, 1);
	/* PXMLT: 新設定を無効にしたとき、地デジ・衛星とも従来値を維持する。 */
	check_clock(false, CXD2856ER_ISDB_T_SYSTEM, 0, 2, 16, 2);
	check_clock(false, CXD2856ER_ISDB_S_SYSTEM, 0, 2, 16, 0);
	/* source 設定失敗後に clock を再開して成功扱いにしない。 */
	assert(cxd2856er_set_ts_clock(&demod, CXD2856ER_ISDB_T_SYSTEM) == -EIO);
	assert(mock.clock_enable_writes == 0);
	puts("CXD2856ER TS clock test: passed");
	return 0;
}
