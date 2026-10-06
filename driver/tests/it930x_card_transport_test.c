// SPDX-License-Identifier: GPL-2.0-only
/* 実 IT930x ソースの通常 / 拡張カード経路を模擬 USB で検証する。
 * cc -std=c11 -Wall -Wextra driver/tests/it930x_card_transport_test.c -o /tmp/it930x_card_transport_test
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#undef __linux__
#undef _WIN32
#undef _WIN64
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
struct mutex { int unused; };
#define mutex_init(p) ((void)(p))
#define mutex_destroy(p) ((void)(p))
#define mutex_lock(p) ((void)(p))
#define mutex_unlock(p) ((void)(p))
#define GFP_KERNEL 0
#define kzalloc(n, f) calloc(1, n)
#define kmalloc(n, f) malloc(n)
#define kfree(p) free(p)
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define dev_err(...) ((void)0)
#define dev_dbg(...) ((void)0)
#define dev_info(...) ((void)0)
#define dev_warn(...) ((void)0)
static void msleep(unsigned int ms) { (void)ms; }
struct firmware { size_t size; const u8 *data; };
static int request_firmware(const struct firmware **f, const char *n, void *d)
{ (void)f; (void)n; (void)d; return -ENOENT; }
static void release_firmware(const struct firmware *f) { (void)f; }
#include "../it930x.c"

static u8 regs[65536], tx[256];
static unsigned int writes[65536];
static u16 available;
static int fail_command = -1;
static bool short_response, short_length;
static unsigned int uart_reads, max_read, mode_count;
static u8 modes[8];
static int mock_tx(struct itedtv_bus *bus, void *buf, int len)
{
	(void)bus;
	memcpy(tx, buf, len);
	if (((tx[1] << 8) | tx[2]) == fail_command) return -EIO;
	if (tx[2] == IT930X_CMD_UART_SET_MODE) modes[mode_count++] = tx[4];
	return 0;
}
static int mock_rx(struct itedtv_bus *bus, void *buf, int *len)
{
	u8 *out = buf;
	u16 cmd = (tx[1] << 8) | tx[2], checksum;
	unsigned int count = 0, reg = 0;
	(void)bus;
	out[1] = tx[3]; out[2] = 0;
	if (cmd == IT930X_CMD_REG_READ || cmd == IT930X_CMD_REG_WRITE) {
		reg = ((u32)tx[6] << 24) | ((u32)tx[7] << 16) | (tx[8] << 8) | tx[9];
		assert(reg + tx[4] <= sizeof(regs));
		if (cmd == IT930X_CMD_REG_WRITE) {
			memcpy(regs + reg, tx + 10, tx[4]); writes[reg]++;
		} else {
			count = tx[4];
			if (reg == 0x4956) { regs[reg] = available >> 8; regs[reg+1] = available & 255; }
			if (reg == IT930X_REG_UART_RX_LENGTH) regs[reg] = available;
			memcpy(out + 3, regs + reg, count);
			if (reg == 0x4956 && short_length) count--;
		}
	} else if (cmd == IT930X_CMD_UART_READ) {
		count = tx[4]; assert(count <= available && count <= 32);
		available -= count; uart_reads++;
		if (count > max_read) max_read = count;
		memset(out + 3, 0xa5, count);
		if (short_response) count--;
	}
	*len = count + 5; out[0] = *len - 1;
	checksum = it930x_calc_checksum(out + 1, *len - 3);
	out[*len-2] = checksum >> 8; out[*len-1] = checksum & 255;
	return 0;
}

int main(void)
{
	struct it930x_bridge bridge = {0};
	u8 data[255], len;
	bool ready = false, detected = false;
	bridge.bus.ops.ctrl_tx = mock_tx;
	bridge.bus.ops.ctrl_rx = mock_rx;
	assert(it930x_init(&bridge) == 0);
	/* 初期化失敗を成功扱いにせず、通常経路を維持する。 */
	fail_command = IT930X_CMD_UART_SET_MODE;
	assert(it930x_bcas_init_extended(&bridge) == -EIO);
	assert(!((struct it930x_priv *)bridge.priv)->bcas_extended);
	fail_command = -1;
	assert(it930x_bcas_init_extended(&bridge) == 0);
	assert(mode_count == 2 && modes[0] == 2 && modes[1] == 5);
	assert(regs[0xd8e8] == 0 && regs[0xd8e9] == 1);
	regs[0xd8e6] = 0;
	assert(it930x_bcas_detect_card(&bridge, &detected) == 0 && detected);
	regs[0xd8e6] = 1;
	assert(it930x_bcas_detect_card(&bridge, &detected) == 0 && !detected);
	assert(it930x_bcas_reset_card(&bridge) == 0);
	assert(writes[0xd8af] == 2 && regs[0xd8af] == 1);
	assert(writes[0xd8e3] == 0 && writes[0x7904] == 0);
	short_length = true;
	assert(it930x_bcas_check_ready(&bridge, &ready) == -EBADMSG);
	short_length = false;
	/* 256以上の受信長と caller の容量を扱い、境界内で読み出す。 */
	available = 300;
	assert(it930x_bcas_check_ready(&bridge, &ready) == 0 && ready);
	len = sizeof(data);
	assert(it930x_bcas_get_data(&bridge, data, &len) == 0);
	assert(len == 255 && available == 45 && uart_reads == 8 && max_read == 32);
	available = 3; short_response = true; len = sizeof(data);
	assert(it930x_bcas_get_data(&bridge, data, &len) == -EBADMSG);
	short_response = false;
	assert(it930x_bcas_send_data(&bridge, data, 49) == 0);
	assert(writes[0x4953] == 1 && writes[IT930X_REG_UART_REALSEND] == 1);
	/* 通常機種は H6 / H14 と既存 UART レジスタを使う。 */
	assert(it930x_bcas_init(&bridge) == 0);
	assert(!((struct it930x_priv *)bridge.priv)->bcas_extended);
	regs[0xd8c6] = 0;
	assert(it930x_bcas_detect_card(&bridge, &detected) == 0 && detected);
	assert(it930x_bcas_reset_card(&bridge) == 0);
	assert(writes[0xd8e3] == 2 && writes[0x7904] == 1);
	regs[IT930X_REG_UART_RX_READY] = 1;
	assert(it930x_bcas_check_ready(&bridge, &ready) == 0 && ready);
	assert(it930x_bcas_send_data(&bridge, data, 1) == 0 && writes[0x4953] == 1);
	it930x_term(&bridge);
	puts("IT930x card transport test: passed");
	return 0;
}
