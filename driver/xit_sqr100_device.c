// SPDX-License-Identifier: GPL-2.0-only
/*
 * PTX driver for XIT XIT-SQR100 device (xit_sqr100_device.c)
 *
 * TS input port 0 and I2C bus 3 / addresses have been checked against
 * a Windows USB capture and Linux reception on XIT-SQR100. The board power
 * GPIO assignments and signal polarity still require independent validation.
 *
 * The implementation below follows the structure of pxmlt_device.c
 * (CXD2856ER demod + tuner pairing) and
 * s1ur_device.c (single-tuner ISDB-T/S structure, plain 0x47 TS sync).
 * All hardware-specific TODO markers flag values that must be confirmed
 * on the target board before the driver is considered functional.
 */

#include "print_format.h"
#include "xit_sqr100_device.h"

#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/slab.h>

#include "px4_device_params.h"
#include "firmware.h"
#include "ts_sync.h"

#define XITSQR100_DEVICE_TS_SYNC_COUNT	4
#define XITSQR100_DEVICE_TS_SYNC_SIZE	(188 * XITSQR100_DEVICE_TS_SYNC_COUNT)

struct xit_sqr100_stream_context {
	struct ptx_chrdev *chrdev;
	u8 remain_buf[XITSQR100_DEVICE_TS_SYNC_SIZE];
	size_t remain_len;
};

static void xit_sqr100_device_release(struct kref *kref);

static int xit_sqr100_backend_set_power(struct xit_sqr100_device *xit,
					bool state)
{
	int ret = 0;
	struct it930x_bridge *it930x = &xit->it930x;

	dev_dbg(xit->dev,
		"xit_sqr100_backend_set_power: %s\n", (state) ? "true" : "false");

	if (!state && !atomic_read(&xit->available))
		return 0;

	if (state) {
		ret = it930x_write_gpio(it930x, 5, true);
		if (ret)
			return ret;

		msleep(50);

		ret = it930x_write_gpio(it930x, 2, false);
		if (ret)
			return ret;

		msleep(100);

		ret = it930x_write_gpio(it930x, 2, true);
		if (ret)
			return ret;

		msleep(20);
	} else {
		it930x_write_gpio(it930x, 5, false);
		it930x_write_gpio(it930x, 2, false);
	}

	return 0;
}

static void xit_sqr100_device_stream_process(struct ptx_chrdev *chrdev,
					     u8 **buf, u32 *len)
{
	u8 *p = *buf;
	u32 remain = *len;

	while (likely(remain)) {
		u32 i = 0;
		bool sync_remain = false;

		/*
		 * Scan for plain 0x47 sync byte (ISDB-T/S standard TS format).
		 * Unlike pxmlt_device.c which uses receiver-numbered TS,
		 * The IT930x input is configured to output plain 0x47.
		 * See ts_sync.h for px4_ts_has_plain_sync().
		 */
		while (true) {
			if (likely(((i + 1) * 188) <= remain)) {
				if (unlikely(!px4_ts_has_plain_sync(p[i * 188])))
					break;
			} else {
				sync_remain = true;
				break;
			}
			i++;
		}

		if (unlikely(i < XITSQR100_DEVICE_TS_SYNC_COUNT)) {
			p++;
			remain--;
			continue;
		}

		ptx_chrdev_put_stream(chrdev, p, 188 * i);

		p += 188 * i;
		remain -= 188 * i;

		if (unlikely(sync_remain))
			break;
	}

	*buf = p;
	*len = remain;
}

static int xit_sqr100_device_stream_handler(void *context, void *buf, u32 len)
{
	struct xit_sqr100_stream_context *stream_ctx = context;
	u8 *ctx_remain_buf = stream_ctx->remain_buf;
	u32 ctx_remain_len = stream_ctx->remain_len;
	u8 *p = buf;
	u32 remain = len;

	if (unlikely(ctx_remain_len)) {
		if (likely((ctx_remain_len + len) >= XITSQR100_DEVICE_TS_SYNC_SIZE)) {
			u32 t = XITSQR100_DEVICE_TS_SYNC_SIZE - ctx_remain_len;

			memcpy(ctx_remain_buf + ctx_remain_len, p, t);
			ctx_remain_len = XITSQR100_DEVICE_TS_SYNC_SIZE;

			xit_sqr100_device_stream_process(stream_ctx->chrdev,
							 &ctx_remain_buf,
							 &ctx_remain_len);
			if (likely(!ctx_remain_len)) {
				p += t;
				remain -= t;
			}

			stream_ctx->remain_len = 0;
		} else {
			memcpy(ctx_remain_buf + ctx_remain_len, p, len);
			stream_ctx->remain_len += len;

			return 0;
		}
	}

	xit_sqr100_device_stream_process(stream_ctx->chrdev, &p, &remain);

	if (unlikely(remain)) {
		memcpy(stream_ctx->remain_buf, p, remain);
		stream_ctx->remain_len = remain;
	}

	return 0;
}

static int xit_sqr100_chrdev_init(struct ptx_chrdev *chrdev)
{
	dev_dbg(chrdev->parent->dev, "xit_sqr100_chrdev_init\n");

	/* 地デジ・衛星の共用入力は周波数番号から方式を判別する。 */
	chrdev->params.system = PTX_UNSPECIFIED_SYSTEM;
	return 0;
}

static int xit_sqr100_chrdev_term(struct ptx_chrdev *chrdev)
{
	dev_dbg(chrdev->parent->dev, "xit_sqr100_chrdev_term\n");
	return 0;
}

static int xit_sqr100_set_demod_init_sleep(struct xit_sqr100_chrdev *chrdevs,
					   bool sleep)
{
	/* Sony の初期化コールバックは SLVX の 0x08 へ sleep 状態を直接書き込む。 */
	return cxd2856er_write_slvx_reg(&chrdevs->cxd2856er, 0x08,
					(sleep) ? 0x01 : 0x00);
}

static int xit_sqr100_chrdev_open(struct ptx_chrdev *chrdev)
{
	int ret = 0;
	struct ptx_chrdev_group *chrdev_group = chrdev->parent;
	struct xit_sqr100_chrdev *chrdevs = chrdev->priv;
	struct xit_sqr100_device *xit = chrdevs->parent;

	dev_dbg(xit->dev,
		"xit_sqr100_chrdev_open %u\n", chrdev_group->id);

	mutex_lock(&xit->lock);

	if (!xit->open_count) {
		ret = xit_sqr100_backend_set_power(xit, true);
		if (ret) {
			dev_err(xit->dev,
				"xit_sqr100_chrdev_open %u: xit_sqr100_backend_set_power(true) failed. (ret: %d)\n",
				chrdev_group->id, ret);
			goto fail_backend_power;
		}
	}

	ret = cxd2856er_init(&chrdevs->cxd2856er);
	if (ret) {
		dev_err(xit->dev,
			"xit_sqr100_chrdev_open %u: cxd2856er_init() failed. (ret: %d)\n",
			chrdev_group->id, ret);
		goto fail_demod_init;
	}

	ret = xit_sqr100_set_demod_init_sleep(chrdevs, true);
	if (ret) {
		dev_err(xit->dev,
			"xit_sqr100_chrdev_open %u: CXD2856ER init sleep failed. (ret: %d)\n",
			chrdev_group->id, ret);
		goto fail_demod_init;
	}

	mutex_lock(chrdevs->tuner_lock);
	ret = cxd6866_init(&chrdevs->cxd6866);
	mutex_unlock(chrdevs->tuner_lock);

	if (ret) {
		dev_err(xit->dev,
			"xit_sqr100_chrdev_open %u: cxd6866_init() failed. (ret: %d)\n",
			chrdev_group->id, ret);
		goto fail_tuner_init;
	}

	ret = xit_sqr100_set_demod_init_sleep(chrdevs, false);
	if (ret) {
		dev_err(xit->dev,
			"xit_sqr100_chrdev_open %u: CXD2856ER init wakeup failed. (ret: %d)\n",
			chrdev_group->id, ret);
		goto fail_tuner_init;
	}

	/* CXD2856ER initialization sequence */
	ret = cxd2856er_write_slvt_reg(&chrdevs->cxd2856er, 0x00, 0x00);
	if (ret)
		goto fail_backend;

	ret = cxd2856er_write_slvt_reg_mask(&chrdevs->cxd2856er,
					    0xc4, 0x80, 0x88);
	if (ret)
		goto fail_backend;


	ret = cxd2856er_write_slvt_reg(&chrdevs->cxd2856er, 0x00, 0xa0);
	if (ret)
		goto fail_backend;

	ret = cxd2856er_write_slvt_reg_mask(&chrdevs->cxd2856er,
					    0xb9, 0x01, 0x01);
	if (ret)
		goto fail_backend;

	xit->open_count++;
	kref_get(&xit->kref);

	mutex_unlock(&xit->lock);
	return 0;

fail_backend:
	mutex_lock(chrdevs->tuner_lock);
	cxd6866_term(&chrdevs->cxd6866);
	mutex_unlock(chrdevs->tuner_lock);

fail_tuner_init:
	cxd2856er_term(&chrdevs->cxd2856er);

fail_demod_init:
	if (!xit->open_count)
		xit_sqr100_backend_set_power(xit, false);

fail_backend_power:
	mutex_unlock(&xit->lock);
	dev_dbg(xit->dev,
		"xit_sqr100_chrdev_open %u: ret: %d\n",
		chrdev_group->id, ret);
	return ret;
}

static int xit_sqr100_chrdev_release(struct ptx_chrdev *chrdev)
{
	struct ptx_chrdev_group *chrdev_group = chrdev->parent;
	struct xit_sqr100_chrdev *chrdevs = chrdev->priv;
	struct xit_sqr100_device *xit = chrdevs->parent;

	dev_dbg(xit->dev,
		"xit_sqr100_chrdev_release %u\n", chrdev_group->id);

	mutex_lock(&xit->lock);

	if (!xit->open_count) {
		mutex_unlock(&xit->lock);
		return -EALREADY;
	}

	mutex_lock(chrdevs->tuner_lock);
	cxd6866_term(&chrdevs->cxd6866);
	mutex_unlock(chrdevs->tuner_lock);

	cxd2856er_term(&chrdevs->cxd2856er);

	xit->open_count--;
	if (!xit->open_count)
		xit_sqr100_backend_set_power(xit, false);

	if (kref_put(&xit->kref, xit_sqr100_device_release))
		return 0;

	mutex_unlock(&xit->lock);
	return 0;
}

static int xit_sqr100_chrdev_tune(struct ptx_chrdev *chrdev,
				  struct ptx_tune_params *params)
{
	int ret = 0;
	struct ptx_chrdev_group *chrdev_group = chrdev->parent;
	struct xit_sqr100_chrdev *chrdevs = chrdev->priv;
	struct xit_sqr100_device *xit = chrdevs->parent;
	union cxd2856er_system_params demod_params;

	dev_dbg(xit->dev,
		"xit_sqr100_chrdev_tune %u\n", chrdev_group->id);

	memset(&demod_params, 0, sizeof(demod_params));

	/* Demodulator wakeup (both ISDB-T and ISDB-S via demod) */
	switch (params->system) {
	case PTX_ISDB_T_SYSTEM:
		demod_params.bandwidth = params->bandwidth;

		ret = cxd2856er_wakeup(&chrdevs->cxd2856er,
				       CXD2856ER_ISDB_T_SYSTEM, &demod_params);
		if (ret)
			dev_err(xit->dev,
				"xit_sqr100_chrdev_tune %u: cxd2856er_wakeup(CXD2856ER_ISDB_T_SYSTEM) failed. (ret: %d)\n",
				chrdev_group->id, ret);
		break;

	case PTX_ISDB_S_SYSTEM:
		demod_params.bandwidth = 0;

		ret = cxd2856er_wakeup(&chrdevs->cxd2856er,
				       CXD2856ER_ISDB_S_SYSTEM, &demod_params);
		if (ret)
			dev_err(xit->dev,
				"xit_sqr100_chrdev_tune %u: cxd2856er_wakeup(CXD2856ER_ISDB_S_SYSTEM) failed. (ret: %d)\n",
				chrdev_group->id, ret);
		break;

	default:
		ret = -EINVAL;
		break;
	}

	if (ret)
		return ret;

	/* Tuner wiring (tuner_lock protects the tuner I2C shared resource) */
	mutex_lock(chrdevs->tuner_lock);

	switch (params->system) {
	case PTX_ISDB_T_SYSTEM:
		ret = cxd6866_set_params_t(&chrdevs->cxd6866,
					   CXD6866_ISDB_T_SYSTEM,
					   params->freq, 6);
		if (ret)
			dev_err(xit->dev,
				"xit_sqr100_chrdev_tune %u: cxd6866_set_params_t(%u, 6) failed. (ret: %d)\n",
				chrdev_group->id, params->freq, ret);
		break;

	case PTX_ISDB_S_SYSTEM:
		ret = cxd6866_set_params_s(&chrdevs->cxd6866,
					   CXD6866_ISDB_S_SYSTEM,
					   params->freq, 28860);
		if (ret)
			dev_err(xit->dev,
				"xit_sqr100_chrdev_tune %u: cxd6866_set_params_s(%u, 28860) failed. (ret: %d)\n",
				chrdev_group->id, params->freq, ret);
		break;

	default:
		break;
	}

	mutex_unlock(chrdevs->tuner_lock);

	if (ret)
		return ret;

	ret = cxd2856er_post_tune(&chrdevs->cxd2856er);
	if (ret) {
		dev_err(xit->dev,
			"xit_sqr100_chrdev_tune %u: cxd2856er_post_tune() failed. (ret: %d)\n",
			chrdev_group->id, ret);
		return ret;
	}

	return 0;
}

static int xit_sqr100_chrdev_check_lock(struct ptx_chrdev *chrdev, bool *locked)
{
	int ret = 0;
	struct xit_sqr100_chrdev *chrdevs = chrdev->priv;
	bool unlocked = false;

	switch (chrdev->current_system) {
	case PTX_ISDB_T_SYSTEM:
		ret = cxd2856er_is_ts_locked_isdbt(&chrdevs->cxd2856er,
						   locked, &unlocked);
		if (!ret && unlocked)
			ret = -ECANCELED;
		break;

	case PTX_ISDB_S_SYSTEM:
		ret = cxd2856er_is_ts_locked_isdbs(&chrdevs->cxd2856er, locked);
		break;

	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int xit_sqr100_chrdev_set_stream_id(struct ptx_chrdev *chrdev, u16 stream_id)
{
	int ret = 0;
	struct ptx_chrdev_group *chrdev_group = chrdev->parent;
	struct xit_sqr100_chrdev *chrdevs = chrdev->priv;
	struct xit_sqr100_device *xit = chrdevs->parent;

	dev_dbg(xit->dev,
		"xit_sqr100_chrdev_set_stream_id %u\n", chrdev_group->id);

	/* ISDB-S stream ID management (slot < 12 or TSID >= 12) */
	if (stream_id < 12) {
		ret = cxd2856er_set_slot_isdbs(&chrdevs->cxd2856er, stream_id);
		if (ret)
			dev_err(xit->dev,
				"xit_sqr100_chrdev_set_stream_id %u: cxd2856er_set_slot_isdbs(%u) failed. (ret: %d)\n",
				chrdev_group->id, stream_id, ret);
	} else {
		ret = cxd2856er_set_tsid_isdbs(&chrdevs->cxd2856er, stream_id);
		if (ret)
			dev_err(xit->dev,
				"xit_sqr100_chrdev_set_stream_id %u: cxd2856er_set_tsid_isdbs(%u) failed. (ret: %d)\n",
				chrdev_group->id, stream_id, ret);
	}

	return ret;
}

static int xit_sqr100_chrdev_start_capture(struct ptx_chrdev *chrdev)
{
	int ret = 0;
	struct ptx_chrdev_group *chrdev_group = chrdev->parent;
	struct xit_sqr100_chrdev *chrdevs = chrdev->priv;
	struct xit_sqr100_device *xit = chrdevs->parent;

	dev_dbg(xit->dev,
		"xit_sqr100_chrdev_start_capture %u\n", chrdev_group->id);

	mutex_lock(&xit->lock);

	if (!xit->streaming_count) {
		struct xit_sqr100_stream_context *stream_ctx = xit->stream_ctx;

		ret = it930x_purge_psb(&xit->it930x,
				       px4_device_params.psb_purge_timeout);
		if (ret) {
			dev_err(xit->dev,
				"xit_sqr100_chrdev_start_capture %u: it930x_purge_psb() failed. (ret: %d)\n",
				chrdev_group->id, ret);
			goto exit;
		}

		stream_ctx->remain_len = 0;

		ret = itedtv_bus_start_streaming(&xit->it930x.bus,
						 xit_sqr100_device_stream_handler,
						 stream_ctx);
		if (ret) {
			dev_err(xit->dev,
				"xit_sqr100_chrdev_start_capture %u: itedtv_bus_start_streaming() failed. (ret: %d)\n",
				chrdev_group->id, ret);
			goto exit;
		}
	}

	xit->streaming_count++;

	dev_dbg(xit->dev,
		"xit_sqr100_chrdev_start_capture %u: streaming_count: %u\n",
		chrdev_group->id, xit->streaming_count);

exit:
	mutex_unlock(&xit->lock);
	return ret;
}

static int xit_sqr100_chrdev_stop_capture(struct ptx_chrdev *chrdev)
{
	struct ptx_chrdev_group *chrdev_group = chrdev->parent;
	struct xit_sqr100_chrdev *chrdevs = chrdev->priv;
	struct xit_sqr100_device *xit = chrdevs->parent;

	dev_dbg(xit->dev,
		"xit_sqr100_chrdev_stop_capture %u\n", chrdev_group->id);

	mutex_lock(&xit->lock);

	if (!xit->streaming_count) {
		mutex_unlock(&xit->lock);
		return -EALREADY;
	}

	xit->streaming_count--;
	if (!xit->streaming_count) {
		dev_dbg(xit->dev,
			"xit_sqr100_chrdev_stop_capture %u: stopping...\n",
			chrdev_group->id);
		itedtv_bus_stop_streaming(&xit->it930x.bus);
	} else {
		dev_dbg(xit->dev,
			"xit_sqr100_chrdev_stop_capture %u: streaming_count: %u\n",
			chrdev_group->id, xit->streaming_count);
	}

	mutex_unlock(&xit->lock);
	return 0;
}

static int xit_sqr100_chrdev_set_capture(struct ptx_chrdev *chrdev, bool status)
{
	return (status) ? xit_sqr100_chrdev_start_capture(chrdev)
			: xit_sqr100_chrdev_stop_capture(chrdev);
}

/* ISDB-T CNR lookup table (10 to 34 dB, 0.5 dB unit) */
static const struct {
	u16 val;
	u32 cn;
} isdbt_cn_raw_table[] = {
	{ 0x51, 0xb19ff }, { 0x5a, 0x9eecd }, { 0x65, 0x8cd8b },
	{ 0x72, 0x7c302 }, { 0x7f, 0x6f132 }, { 0x8f, 0x6250d },
	{ 0xa0, 0x57a1c }, { 0xb4, 0x4db45 }, { 0xc9, 0x45725 },
	{ 0xe2, 0x3da59 }, { 0xfd, 0x36f9d }, { 0x11c, 0x30e58 },
	{ 0x13f, 0x2b76e }, { 0x166, 0x26abb }, { 0x191, 0x22794 },
	{ 0x1c2, 0x1eac7 }, { 0x1f9, 0x1b4a8 }, { 0x237, 0x1844d },
	{ 0x27c, 0x159a2 }, { 0x2ca, 0x13365 }, { 0x321, 0x11196 },
	{ 0x382, 0xf3ae }, { 0x3f0, 0xd8cb }, { 0x46b, 0xc0fd },
	{ 0x4f4, 0xabf9 }, { 0x58f, 0x9923 }, { 0x63d, 0x8868 },
	{ 0x700, 0x7995 }, { 0x7da, 0x6c78 }, { 0x8cf, 0x60cf },
	{ 0x9e2, 0x5675 }, { 0xb17, 0x4d43 }, { 0xc71, 0x4520 },
	{ 0xdf6, 0x3de4 }, { 0xfaa, 0x377b }, { 0x1193, 0x31cc },
	{ 0x13b8, 0x2cbf }, { 0x1620, 0x2843 }, { 0x18d3, 0x2447 },
	{ 0x1bdb, 0x20bb }, { 0x1f41, 0x1d95 }, { 0x2311, 0x1ac6 },
	{ 0x2758, 0x1846 }, { 0x2c25, 0x160a }, { 0x3188, 0x140c },
	{ 0x3793, 0x1243 }, { 0x3e5b, 0x10ab }, { 0x45f7, 0xf3c },
	{ 0x4e80, 0xdf3 }
};

/* ISDB-S CNR lookup table (0 to 20 dB, 0.1 dB unit) */
static const struct {
	u16 val;
	u32 cn;
} isdbs_cn_raw_table[] = {
	{ 0x5af, 0x9546 }, { 0x597, 0x94d9 }, { 0x57e, 0x946b },
	{ 0x567, 0x93fc }, { 0x550, 0x938c }, { 0x539, 0x931b },
	{ 0x522, 0x92a8 }, { 0x50c, 0x9235 }, { 0x4f6, 0x91c1 },
	{ 0x4e1, 0x914b }, { 0x4cc, 0x90d5 }, { 0x4b6, 0x905d },
	{ 0x4a1, 0x8fe4 }, { 0x48c, 0x8f6a }, { 0x477, 0x8eef },
	{ 0x463, 0x8e72 }, { 0x44f, 0x8df5 }, { 0x43c, 0x8d76 },
	{ 0x428, 0x8cf5 }, { 0x416, 0x8c74 }, { 0x403, 0x8bf1 },
	{ 0x3ef, 0x8b6c }, { 0x3dc, 0x8ae7 }, { 0x3c9, 0x8a60 },
	{ 0x3b6, 0x89d7 }, { 0x3a4, 0x894d }, { 0x392, 0x88c2 },
	{ 0x381, 0x8835 }, { 0x36f, 0x87a6 }, { 0x35f, 0x8716 },
	{ 0x34e, 0x8685 }, { 0x33d, 0x85f1 }, { 0x32d, 0x855d },
	{ 0x31d, 0x84c6 }, { 0x30d, 0x842e }, { 0x2fd, 0x8394 },
	{ 0x2ee, 0x82f9 }, { 0x2df, 0x825b }, { 0x2d0, 0x81bc },
	{ 0x2c2, 0x811c }, { 0x2b4, 0x8079 }, { 0x2a6, 0x7fd5 },
	{ 0x299, 0x7f2f }, { 0x28c, 0x7e87 }, { 0x27f, 0x7ddd },
	{ 0x272, 0x7d31 }, { 0x265, 0x7c83 }, { 0x259, 0x7bd4 },
	{ 0x24d, 0x7b22 }, { 0x241, 0x7a6f }, { 0x236, 0x79ba },
	{ 0x22b, 0x7903 }, { 0x220, 0x784a }, { 0x215, 0x778f },
	{ 0x20a, 0x76d3 }, { 0x200, 0x7614 }, { 0x1f6, 0x7554 },
	{ 0x1ec, 0x7492 }, { 0x1e2, 0x73ce }, { 0x1d8, 0x7308 },
	{ 0x1cf, 0x7241 }, { 0x1c6, 0x7178 }, { 0x1bc, 0x70ad },
	{ 0x1b3, 0x6fe1 }, { 0x1aa, 0x6f13 }, { 0x1a2, 0x6e44 },
	{ 0x199, 0x6d74 }, { 0x191, 0x6ca2 }, { 0x189, 0x6bcf },
	{ 0x181, 0x6afb }, { 0x179, 0x6a26 }, { 0x171, 0x6950 },
	{ 0x169, 0x687a }, { 0x161, 0x67a2 }, { 0x15a, 0x66ca },
	{ 0x153, 0x65f1 }, { 0x14b, 0x6517 }, { 0x144, 0x643e },
	{ 0x13d, 0x6364 }, { 0x137, 0x628a }, { 0x130, 0x61b0 },
	{ 0x129, 0x60d5 }, { 0x123, 0x5ffa }, { 0x11c, 0x5f1e },
	{ 0x116, 0x5e42 }, { 0x10f, 0x5d66 }, { 0x109, 0x5c8a },
	{ 0x103, 0x5bad }
};

static int xit_sqr100_chrdev_read_cnr_raw(struct ptx_chrdev *chrdev, u32 *value)
{
	int ret = 0;
	struct xit_sqr100_chrdev *chrdevs = chrdev->priv;

	switch (chrdev->current_system) {
	case PTX_ISDB_T_SYSTEM:
	{
		int i, i_min, i_max;
		u16 val;

		ret = cxd2856er_read_cnr_raw_isdbt(&chrdevs->cxd2856er, &val);
		if (ret)
			break;

		i_min = 0;
		i_max = ARRAY_SIZE(isdbt_cn_raw_table) - 1;

		if (isdbt_cn_raw_table[i_min].val >= val) {
			*value = isdbt_cn_raw_table[i_min].cn;
			break;
		}
		if (isdbt_cn_raw_table[i_max].val <= val) {
			*value = isdbt_cn_raw_table[i_max].cn;
			break;
		}

		while (1) {
			i = i_min + (i_max - i_min) / 2;

			if (isdbt_cn_raw_table[i].val == val) {
				*value = isdbt_cn_raw_table[i].cn;
				break;
			}

			if (isdbt_cn_raw_table[i].val < val)
				i_min = i + 1;
			else
				i_max = i - 1;

			if (i_max < i_min) {
				*value = isdbt_cn_raw_table[i_max].cn;
				break;
			}
		}

		break;
	}

	case PTX_ISDB_S_SYSTEM:
	{
		int i, i_min, i_max;
		u16 val;

		ret = cxd2856er_read_cnr_raw_isdbs(&chrdevs->cxd2856er, &val);
		if (ret)
			break;

		i_min = 0;
		i_max = ARRAY_SIZE(isdbs_cn_raw_table) - 1;

		if (isdbs_cn_raw_table[i_min].val <= val) {
			*value = isdbs_cn_raw_table[i_min].cn;
			break;
		}
		if (isdbs_cn_raw_table[i_max].val >= val) {
			*value = isdbs_cn_raw_table[i_max].cn;
			break;
		}

		while (1) {
			i = i_min + (i_max - i_min) / 2;

			if (isdbs_cn_raw_table[i].val == val) {
				*value = isdbs_cn_raw_table[i].cn;
				break;
			}

			if (isdbs_cn_raw_table[i].val > val)
				i_min = i + 1;
			else
				i_max = i - 1;

			if (i_max < i_min) {
				*value = isdbs_cn_raw_table[i_max].cn;
				break;
			}
		}

		break;
	}

	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static struct ptx_chrdev_operations xit_sqr100_chrdev_ops = {
	.init = xit_sqr100_chrdev_init,
	.term = xit_sqr100_chrdev_term,
	.open = xit_sqr100_chrdev_open,
	.release = xit_sqr100_chrdev_release,
	.tune = xit_sqr100_chrdev_tune,
	.check_lock = xit_sqr100_chrdev_check_lock,
	.set_stream_id = xit_sqr100_chrdev_set_stream_id,
	.set_lnb_voltage = NULL,
	.set_capture = xit_sqr100_chrdev_set_capture,
	.read_signal_strength = NULL,
	.read_cnr = NULL,
	.read_cnr_raw = xit_sqr100_chrdev_read_cnr_raw
};

static int xit_sqr100_device_load_config(struct xit_sqr100_device *xit,
					 struct ptx_chrdev_config *chrdev_config)
{
	int ret = 0;
	struct device *dev = xit->dev;
	struct it930x_bridge *it930x = &xit->it930x;
	struct it930x_stream_input *input = &it930x->config.input[0];
	struct xit_sqr100_chrdev *chrdevs = &xit->chrdevs;
	int i;
	u8 tmp;

	ret = it930x_read_reg(it930x, 0x4979, &tmp);
	if (ret) {
		dev_err(dev,
			"xit_sqr100_load_config: it930x_read_reg(0x4979) failed.\n");
		return ret;
	} else if (!tmp) {
		dev_warn(dev, "EEPROM error.\n");
		return ret;
	}

	chrdev_config->system_cap = PTX_ISDB_T_SYSTEM | PTX_ISDB_S_SYSTEM;

	input->enable = true;
	input->is_parallel = false;
	/*
	 * IT9300BDA.sys addresses the CXD2856ER with 8-bit chip_type 0xC8;
	 * the Linux I2C API uses the corresponding 7-bit address 0x64.
	 * PXMLT devices using the same demodulator also use 0x64/0x65/0x6c,
	 * with SLVX at SLVT + 2. The Windows binary evidence identifies the
	 * address; the Windows USB capture and Linux reception also confirm
	 * bus 3 and these addresses.
	 */
	/*
	 * Windows の実機キャプチャは 0xda58/0xda73/0xda4c を設定し、
	 * serial TS の port 0 を使用する。無効入力も別の port を指す
	 * 必要があり、ゼロ初期値のままだと後続の設定で port 0 が無効になる。
	 */
	for (i = 0; i < 5; i++)
		it930x->config.input[i].port_number = i;
	input->port_number = 0;
	input->slave_number = 0;
	input->i2c_bus = 3;
	input->i2c_addr = 0x64;
	input->packet_len = 188;
	input->sync_byte = 0x47;	/* Plain ISDB-T/S sync byte */

	chrdevs->cxd2856er.dev = dev;
	chrdevs->cxd2856er.i2c = &it930x->i2c_master[input->i2c_bus - 1];
	chrdevs->cxd2856er.i2c_addr.slvx = input->i2c_addr + 2;	/* 0x66 */
	chrdevs->cxd2856er.i2c_addr.slvt = input->i2c_addr;	/* 0x64 */
	chrdevs->cxd2856er.config.xtal = 24000;
	chrdevs->cxd2856er.config.tuner_i2c = true;
	chrdevs->cxd2856er.config.serial_ts_clock = true;

	/*
	 * CXD6866AER (FREIA) board settings, taken from the XIT-SQR100
	 * Windows driver (IT9300BDA.sys):
	 *   i2c_addr = 0x60 (7-bit; the driver passes the 8-bit form 0xC0,
	 *               connected to the CXD2856ER's I2C master)
	 *   xtal = 24000 kHz (24 MHz crystal; register 0x81 = 0x18 in the
	 *          binary's X_pon)
	 *   loop_filter_internal + REFOUT_800mVpp (config flags 0x10004000)
	 */
	chrdevs->cxd6866.dev = dev;
	chrdevs->cxd6866.i2c = &chrdevs->cxd2856er.i2c_master;
	chrdevs->cxd6866.i2c_addr = 0x60;
	chrdevs->cxd6866.config.xtal = 24000;
	chrdevs->cxd6866.config.loop_filter_internal = true;
	/* 0x4000 は REFOUT。IF 出力制限の 0x40000 は指定されていない。 */
	chrdevs->cxd6866.config.outlmt_dtv_1_2vpp = false;
	/* Windows の実機 USB キャプチャで確認した REFOUT 設定。 */
	chrdevs->cxd6866.config.refout_enable = true;

	return 0;
}

int xit_sqr100_device_init(struct xit_sqr100_device *xit, struct device *dev,
			   struct ptx_chrdev_context *chrdev_ctx,
			   struct completion *quit_completion)
{
	int ret = 0;
	struct it930x_bridge *it930x;
	struct itedtv_bus *bus;
	struct ptx_chrdev_config chrdev_config;
	struct ptx_chrdev_group_config chrdev_group_config;
	struct ptx_chrdev_group *chrdev_group;
	struct xit_sqr100_stream_context *stream_ctx;

	if (!xit || !dev || !chrdev_ctx || !quit_completion)
		return -EINVAL;

	dev_dbg(dev, "xit_sqr100_device_init\n");

	get_device(dev);

	mutex_init(&xit->lock);
	kref_init(&xit->kref);
	xit->dev = dev;
	xit->quit_completion = quit_completion;
	xit->open_count = 0;
	xit->streaming_count = 0;
	mutex_init(&xit->tuner_lock);

	xit->chrdevs.chrdev = NULL;
	xit->chrdevs.parent = xit;
	xit->chrdevs.tuner_lock = &xit->tuner_lock;

	stream_ctx = kzalloc(sizeof(*stream_ctx), GFP_KERNEL);
	if (!stream_ctx) {
		dev_err(xit->dev,
			"xit_sqr100_device_init: kzalloc(sizeof(*stream_ctx), GFP_KERNEL) failed.\n");
		ret = -ENOMEM;
		goto fail;
	}
	xit->stream_ctx = stream_ctx;

	it930x = &xit->it930x;
	bus = &it930x->bus;

	ret = itedtv_bus_init(bus);
	if (ret)
		goto fail_bus;

	ret = it930x_init(it930x);
	if (ret)
		goto fail_bridge;

	ret = it930x_raise(it930x);
	if (ret)
		goto fail_device;

	ret = xit_sqr100_device_load_config(xit, &chrdev_config);
	if (ret)
		goto fail_device;

	chrdev_config.ops = &xit_sqr100_chrdev_ops;
	chrdev_config.options = PTX_CHRDEV_SAT_SET_STREAM_ID_BEFORE_TUNE |
				PTX_CHRDEV_WAIT_AFTER_LOCK_TC_T;
	chrdev_config.ringbuf_size = 188 * px4_device_params.tsdev_max_packets;
	chrdev_config.ringbuf_threshold_size = chrdev_config.ringbuf_size / 10;
	chrdev_config.priv = &xit->chrdevs;

	ret = it930x_load_firmware(it930x, IT930X_FIRMWARE_FILENAME);
	if (ret)
		goto fail_device;

	ret = it930x_init_warm(it930x);
	if (ret)
		goto fail_device;

	ret = it930x_write_reg(it930x, 0xdab0, 1);
	if (ret)
		goto fail_device;
	ret = it930x_write_reg(it930x, 0xdaae, 0);
	if (ret)
		goto fail_device;

	/* GPIO */
	ret = it930x_set_gpio_mode(it930x, 5, IT930X_GPIO_OUT, true);
	if (ret)
		goto fail_device;

	ret = it930x_write_gpio(it930x, 5, false);
	if (ret)
		goto fail_device;

	ret = it930x_set_gpio_mode(it930x, 2, IT930X_GPIO_OUT, true);
	if (ret)
		goto fail_device;

	ret = it930x_write_gpio(it930x, 2, false);
	if (ret)
		goto fail_device;

	ret = it930x_set_gpio_mode(it930x, 7, IT930X_GPIO_OUT, true);
	if (ret)
		goto fail_device;

	ret = it930x_write_gpio(it930x, 7, true);
	if (ret)
		goto fail_device;

	if (px4_device_params.discard_null_packets) {
		struct it930x_pid_filter filter;

		filter.block = true;
		filter.num = 1;
		filter.pid[0] = 0x1fff;

		ret = it930x_set_pid_filter(it930x, 0, &filter);
		if (ret)
			goto fail_device;
	}

	chrdev_group_config.owner_kref = &xit->kref;
	chrdev_group_config.owner_kref_release = xit_sqr100_device_release;
	chrdev_group_config.reserved = false;
	chrdev_group_config.minor_base = 0;	/* unused */
	chrdev_group_config.chrdev_num = XITSQR100_CHRDEV_NUM;
	chrdev_group_config.chrdev_config = &chrdev_config;

	ret = ptx_chrdev_context_add_group(chrdev_ctx, dev,
					   &chrdev_group_config, &chrdev_group);
	if (ret)
		goto fail_chrdev;

	xit->chrdev_group = chrdev_group;
	xit->chrdevs.chrdev = &chrdev_group->chrdev[0];
	stream_ctx->chrdev = &chrdev_group->chrdev[0];

	atomic_set(&xit->available, 1);
	return 0;

fail_chrdev:

fail_device:
	it930x_term(it930x);

fail_bridge:
	itedtv_bus_term(bus);

fail_bus:
	kfree(xit->stream_ctx);

fail:
	mutex_destroy(&xit->tuner_lock);
	mutex_destroy(&xit->lock);
	put_device(dev);
	return ret;
}

static void xit_sqr100_device_release(struct kref *kref)
{
	struct xit_sqr100_device *xit = container_of(kref,
						     struct xit_sqr100_device,
						     kref);

	dev_dbg(xit->dev, "xit_sqr100_device_release\n");

	it930x_term(&xit->it930x);
	itedtv_bus_term(&xit->it930x.bus);

	kfree(xit->stream_ctx);
	put_device(xit->dev);

	complete(xit->quit_completion);
	return;
}

void xit_sqr100_device_term(struct xit_sqr100_device *xit)
{
	dev_dbg(xit->dev,
		"xit_sqr100_device_term: kref count: %u\n",
		kref_read(&xit->kref));

	atomic_xchg(&xit->available, 0);
	ptx_chrdev_group_destroy(xit->chrdev_group);

	kref_put(&xit->kref, xit_sqr100_device_release);
	return;
}
