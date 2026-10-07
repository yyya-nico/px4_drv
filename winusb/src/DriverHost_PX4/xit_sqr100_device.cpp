// xit_sqr100_device.cpp

#include "xit_sqr100_device.hpp"
#include "cxd2856er_cn_table.hpp"

#include <cmath>
#include <iterator>

#include "util.hpp"
#include "ts_sync.h"

namespace px4 {

/* カード検出とリセットの配線・検出極性は機種側で定義する。 */
static const it930x_bcas_config bcas_config = { 15, 1, true };

XitSqr100Device::XitSqr100Device(const std::wstring &path, const DeviceDefinition &definition, std::uintptr_t index, ReceiverManager &receiver_manager)
	: DeviceBase(path, definition, index, receiver_manager)
{
	if (usb_dev_.descriptor.idVendor != 0x06b8 ||
		usb_dev_.descriptor.idProduct != 0x106b)
		throw DeviceError("px4::XitSqr100Device: unsupported device.");
	if (definition.receivers.size() != 1 || definition.receivers[0].index != 0)
		throw DeviceError("px4::XitSqr100Device: exactly one receiver is required.");

	LoadConfig();
}

XitSqr100Device::~XitSqr100Device()
{
	Term();
}

int XitSqr100Device::Init()
{
	std::lock_guard<std::recursive_mutex> lock(lock_);

	if (!available_)
		return -ENODEV;

	if (initialized_)
		return -EALREADY;

	bridge_.dev = &dev_;
	bridge_.bus.dev = &dev_;
	bridge_.bus.type = ITEDTV_BUS_USB;
	bridge_.bus.usb.dev = &usb_dev_;
	bridge_.bus.usb.ctrl_timeout = 3000;
	bridge_.config.xfer_size = 188 * xfer_packets_;
	bridge_.config.i2c_speed = 7;

	for (int i = 0; i < 5; ++i)
		bridge_.config.input[i].port_number = static_cast<u8>(i);

	auto &input = bridge_.config.input[0];
	input.enable = true;
	input.is_parallel = false;
	input.i2c_bus = 3;
	input.i2c_addr = 0x64;
	input.packet_len = 188;
	input.sync_byte = 0x47;

	int ret = itedtv_bus_init(&bridge_.bus);
	if (ret)
		return ret;

	ret = it930x_init(&bridge_);

	if (ret) {
		itedtv_bus_term(&bridge_.bus);
		return ret;
	}

	ret = it930x_raise(&bridge_);

	if (!ret)
		ret = it930x_load_firmware(&bridge_, "it930x-firmware.bin");

	if (!ret)
		ret = it930x_init_warm(&bridge_);

	/* GPIO */
	if (!ret)
		ret = it930x_set_gpio_mode(&bridge_, 5, IT930X_GPIO_OUT, true);

	if (!ret)
		ret = it930x_write_gpio(&bridge_, 5, false);

	if (!ret)
		ret = it930x_set_gpio_mode(&bridge_, 2, IT930X_GPIO_OUT, true);

	if (!ret)
		ret = it930x_write_gpio(&bridge_, 2, false);

	if (ret) {
		it930x_term(&bridge_);
		itedtv_bus_term(&bridge_.bus);
		return ret;
	}

	receiver_.reset(new Receiver(*this));
	stream_ctx_.stream_buf = receiver_->GetStreamBuffer();
	initialized_ = true;

	const auto &definition = device_def_.receivers.front();
	command::ReceiverInfo info = {};
	wcscpy_s(info.device_name, device_def_.name.c_str());
	info.device_guid = device_def_.guid;
	wcscpy_s(info.receiver_name, definition.name.c_str());
	info.receiver_guid = definition.guid;
	info.systems = definition.systems;
	info.index = definition.index;
	receiver_manager_.Register(info, receiver_.get());

	return 0;
}

void XitSqr100Device::Term()
{
	std::unique_lock<std::recursive_mutex> lock(lock_);

	if (!initialized_)
		return;
	terminating_ = true;
	CloseCard();

	/* manager は Open 時に manager -> device の順で取得するため、先に device を解く。 */
	lock.unlock();
	receiver_manager_.Unregister(receiver_.get());

	/* 受信機の利用者が閉じるまでの待機中はデバイスロックを保持しない。 */
	receiver_.reset();
	lock.lock();
	stream_ctx_.stream_buf.reset();
	SetBackendPower(false);
	available_ = false;
	initialized_ = false;
	it930x_term(&bridge_);
	itedtv_bus_term(&bridge_.bus);
}

void XitSqr100Device::SetAvailability(bool available)
{
	std::lock_guard<std::recursive_mutex> lock(lock_);

	if (!available) {
		card_open_ = false;
		if (stream_ctx_.stream_buf)
			stream_ctx_.stream_buf->StopRequest();
	}

	available_ = available;
}

ReceiverBase *XitSqr100Device::GetReceiver(int id) const
{
	if (id != 0)
		throw std::out_of_range("receiver id out of range");

	return receiver_.get();
}

int XitSqr100Device::OpenCard()
{
	std::lock_guard<std::recursive_mutex> lock(lock_);

	if (!available_ || !initialized_ || terminating_)
		return -ENODEV;
	if (card_open_)
		return -EBUSY;

	/* 受信中の H2 パルスはチューナーをリセットするため初回利用だけ投入する。 */
	int ret = receiver_open_ ? 0 : SetBackendPower(true);

	if (ret) {
		SetBackendPower(false);
		return ret;
	}

	/* カード初期化の失敗は受信機の登録や受信を中止させない。 */
	ret = it930x_bcas_init(&bridge_, &bcas_config);
	if (!ret)
		card_open_ = true;
	else if (!receiver_open_)
		SetBackendPower(false);
	return ret;
}

void XitSqr100Device::CloseCard()
{
	std::lock_guard<std::recursive_mutex> lock(lock_);

	if (!card_open_)
		return;

	card_open_ = false;
	if (!receiver_open_)
		SetBackendPower(false);
}

int XitSqr100Device::DetectCard(bool &detected)
{
	std::lock_guard<std::recursive_mutex> lock(lock_);
	return card_open_ && available_ && initialized_ && !terminating_ ?
		it930x_bcas_detect_card(&bridge_, &bcas_config, &detected) : -ENODEV;
}

int XitSqr100Device::ResetCard()
{
	std::lock_guard<std::recursive_mutex> lock(lock_);
	return card_open_ && available_ && initialized_ ?
		it930x_bcas_reset_card(&bridge_, &bcas_config) : -ENODEV;
}

int XitSqr100Device::SetCardBaudrate(::it930x_uart_baudrate baudrate)
{
	std::lock_guard<std::recursive_mutex> lock(lock_);
	return available_ && initialized_ && card_open_ ?
		it930x_bcas_set_baudrate(&bridge_, baudrate) : -ENODEV;
}

int XitSqr100Device::IsCardDataReady(bool &ready)
{
	std::lock_guard<std::recursive_mutex> lock(lock_);
	return available_ && initialized_ && card_open_ ?
		it930x_bcas_check_ready(&bridge_, &ready) : -ENODEV;
}

int XitSqr100Device::ReadCardData(std::uint8_t *buf, std::uint8_t &len)
{
	std::lock_guard<std::recursive_mutex> lock(lock_);
	return available_ && initialized_ && card_open_ ?
		it930x_bcas_get_data(&bridge_, buf, &len) : -ENODEV;
}

int XitSqr100Device::WriteCardData(const std::uint8_t *buf, std::uint8_t len)
{
	std::lock_guard<std::recursive_mutex> lock(lock_);
	return available_ && initialized_ && card_open_ ?
		it930x_bcas_send_data(&bridge_, buf, len) : -ENODEV;
}

void XitSqr100Device::LoadConfig()
{
	const auto &config = device_def_.configs;

	if (config.Exists(L"XferPackets"))
		xfer_packets_ = util::wtoui(config.Get(L"XferPackets"));

	if (config.Exists(L"UrbMaxPackets"))
		urb_packets_ = util::wtoui(config.Get(L"UrbMaxPackets"));

	if (config.Exists(L"MaxUrbs"))
		max_urbs_ = util::wtoui(config.Get(L"MaxUrbs"));

	if (config.Exists(L"ReceiverMaxPackets"))
		buffer_packets_ = util::wtoui(config.Get(L"ReceiverMaxPackets"));

	if (config.Exists(L"PsbPurgeTimeout"))
		purge_timeout_ = util::wtoi(config.Get(L"PsbPurgeTimeout"));

	if (config.Exists(L"NoRawIo"))
		no_raw_io_ = util::wtob(config.Get(L"NoRawIo"));

	/* バイト数への変換の overflow とゼロサイズを機器初期化前に拒否する。 */
	if (!xfer_packets_ || xfer_packets_ > UINT32_MAX / 188 ||
		!urb_packets_ || urb_packets_ > (UINT32_MAX - 512) / 188 || !max_urbs_ || max_urbs_ > 64 ||
		!buffer_packets_ || buffer_packets_ > UINT32_MAX / 188 || purge_timeout_ < 0)
		throw DeviceError("px4::XitSqr100Device: invalid buffer configuration.");
}

int XitSqr100Device::SetBackendPower(bool state)
{
	if (!available_)
		return state ? -ENODEV : 0;

	if (state) {
		int ret = it930x_write_gpio(&bridge_, 5, true);
		if (ret)
			return ret;
		Sleep(50);
		ret = it930x_write_gpio(&bridge_, 2, false);
		if (ret)
			return ret;
		Sleep(100);
		ret = it930x_write_gpio(&bridge_, 2, true);
		if (ret)
			return ret;
		Sleep(20);
	} else {
		/* Linux の停止手順と同じ順序で両方の停止を試みる。 */
		int ret = it930x_write_gpio(&bridge_, 5, false);
		int ret2 = it930x_write_gpio(&bridge_, 2, false);
		return ret ? ret : ret2;
	}

	return 0;
}

void XitSqr100Device::StreamProcess(std::shared_ptr<px4::ReceiverBase::StreamBuffer> stream_buf, std::uint8_t **buf, std::size_t &len)
{
	std::uint8_t *p = *buf;
	std::size_t remain = len;

	while (remain) {
		std::size_t i = 0;
		bool sync_remain = false;

		while (true) {
			if (((i + 1) * 188) <= remain) {
				/* 連続する同期バイトを数え、崩れた位置の直前までを出力する */
				if (!px4_ts_has_plain_sync(p[i * 188]))
					break;
			} else {
				sync_remain = true;
				break;
			}
			i++;
		}

		if (i < XIT_SQR100_DEVICE_TS_SYNC_COUNT) {
			p++;
			remain--;
			continue;
		}

		std::size_t pkt_len = 188 * i;
		stream_buf->Write(p, pkt_len);

		p += 188 * i;
		remain -= 188 * i;

		if (sync_remain)
			break;
	}

	stream_buf->NotifyWrite();

	*buf = p;
	len = remain;

	return;
}

int XitSqr100Device::StreamHandler(void *context, void *buf, std::uint32_t len)
{
	XitSqr100Device &obj = *static_cast<XitSqr100Device*>(context);
	StreamContext &stream_ctx = obj.stream_ctx_;
	std::uint8_t *p = static_cast<std::uint8_t*>(buf);
	std::size_t remain = len;

	if (stream_ctx.remain_len) {
		if ((stream_ctx.remain_len + len) >= XIT_SQR100_DEVICE_TS_SYNC_SIZE) {
			std::uint8_t * remain_buf = stream_ctx.remain_buf;
			std::size_t t = XIT_SQR100_DEVICE_TS_SYNC_SIZE - stream_ctx.remain_len;

			memcpy(remain_buf + stream_ctx.remain_len, p, t);
			stream_ctx.remain_len = XIT_SQR100_DEVICE_TS_SYNC_SIZE;

			StreamProcess(stream_ctx.stream_buf, &remain_buf, stream_ctx.remain_len);
			if (!stream_ctx.remain_len) {
				p += t;
				remain -= t;
			}

			stream_ctx.remain_len = 0;
		} else {
			memcpy(stream_ctx.remain_buf + stream_ctx.remain_len, p, len);
			stream_ctx.remain_len += len;

			return 0;
		}
	}

	StreamProcess(stream_ctx.stream_buf, &p, remain);

	if (remain) {
		memcpy(stream_ctx.remain_buf, p, remain);
		stream_ctx.remain_len = remain;
	}

	return 0;
}

XitSqr100Device::Receiver::Receiver(XitSqr100Device &parent)
	: ReceiverBase(RECEIVER_SAT_SET_STREAM_ID_BEFORE_TUNE | RECEIVER_WAIT_AFTER_LOCK_TC_T),
	parent_(parent)
{
	demod_.dev = &parent_.dev_;
	demod_.i2c = &parent_.bridge_.i2c_master[2];
	demod_.i2c_addr.slvx = 0x66;
	demod_.i2c_addr.slvt = 0x64;
	demod_.config.xtal = 24000;
	demod_.config.tuner_i2c = true;
	demod_.config.serial_ts_clock = true;

	tuner_.dev = &parent_.dev_;
	tuner_.i2c = &demod_.i2c_master;
	tuner_.i2c_addr = 0x60;
	tuner_.config.xtal = 24000;
	tuner_.config.loop_filter_internal = true;
	tuner_.config.refout_enable = true;
}

XitSqr100Device::Receiver::~Receiver()
{
	std::unique_lock<std::recursive_mutex> lock(parent_.lock_);

	close_cond_.wait(lock, [this] { return !parent_.receiver_open_; });
}

int XitSqr100Device::Receiver::Open()
{
	std::lock_guard<std::recursive_mutex> lock(parent_.lock_);

	if (!parent_.available_ || !parent_.initialized_ || parent_.terminating_)
		return -ENODEV;

	if (parent_.receiver_open_)
		return -EALREADY;

	int ret = parent_.card_open_ ? 0 : parent_.SetBackendPower(true);

	if (ret) {
		parent_.SetBackendPower(false);
		return ret;
	}

	bool demod_initialized = false, tuner_initialized = false;
	ret = cxd2856er_init(&demod_);

	if (!ret) {
		demod_initialized = true;
		/* Linux 版と同じ Sony init sleep callback で tuner 初期化を挟む。 */
		ret = cxd2856er_write_slvx_reg(&demod_, 0x08, 1);
	}

	if (!ret) {
		ret = cxd6866_init(&tuner_);
		tuner_initialized = !ret;
	}

	if (!ret)
		ret = cxd2856er_write_slvx_reg(&demod_, 0x08, 0);

	if (!ret)
		ret = cxd2856er_write_slvt_reg(&demod_, 0x00, 0);

	if (!ret)
		ret = cxd2856er_write_slvt_reg_mask(&demod_, 0xc4, 0x80, 0x88);

	if (!ret)
		ret = cxd2856er_write_slvt_reg(&demod_, 0x00, 0xa0);

	if (!ret)
		ret = cxd2856er_write_slvt_reg_mask(&demod_, 0xb9, 1, 1);

	if (ret) {
		if (tuner_initialized)
			cxd6866_term(&tuner_);
		if (demod_initialized)
			cxd2856er_term(&demod_);
		if (!parent_.card_open_)
			parent_.SetBackendPower(false);
		return ret;
	}

	parent_.receiver_open_ = true;

	return 0;
}

void XitSqr100Device::Receiver::Close()
{
	std::unique_lock<std::recursive_mutex> lock(parent_.lock_);

	if (!parent_.receiver_open_)
		return;
	SetCapture(false);
	if (parent_.available_) {
		cxd6866_term(&tuner_);
		cxd2856er_term(&demod_);
	}

	parent_.receiver_open_ = false;
	if (!parent_.card_open_)
		parent_.SetBackendPower(false);
	system_ = SystemType::UNSPECIFIED;
	lock.unlock();
	close_cond_.notify_all();
}

int XitSqr100Device::Receiver::SetFrequency()
{
	std::lock_guard<std::recursive_mutex> lock(parent_.lock_);

	if (!parent_.available_ || !parent_.receiver_open_ || parent_.terminating_)
		return -ENODEV;

	int ret;

	system_ = SystemType::UNSPECIFIED;

	cxd2856er_system_params parameters = {};
	if (params_.system == SystemType::ISDB_T) {
		parameters.bandwidth = params_.bandwidth ? params_.bandwidth : 6;

		ret = cxd2856er_wakeup(&demod_, CXD2856ER_ISDB_T_SYSTEM, &parameters);
		if (!ret)
			ret = cxd6866_set_params_t(&tuner_, CXD6866_ISDB_T_SYSTEM, params_.freq, 6);
	} else if (params_.system == SystemType::ISDB_S) {
		ret = cxd2856er_wakeup(&demod_, CXD2856ER_ISDB_S_SYSTEM, &parameters);
		if (!ret)
			ret = cxd6866_set_params_s(&tuner_, CXD6866_ISDB_S_SYSTEM, params_.freq, 28860);
	} else
		return -EINVAL;

	if (!ret)
		ret = cxd2856er_post_tune(&demod_);

	if (!ret)
		system_ = params_.system;
	return ret;
}

int XitSqr100Device::Receiver::SetStreamId()
{
	std::lock_guard<std::recursive_mutex> lock(parent_.lock_);

	if (!parent_.available_ || !parent_.receiver_open_ || parent_.terminating_)
		return -ENODEV;

	if (params_.system != SystemType::ISDB_S || params_.stream_id > UINT16_MAX)
		return -EINVAL;

	return params_.stream_id < 12 ? cxd2856er_set_slot_isdbs(&demod_, static_cast<u16>(params_.stream_id)) :
		cxd2856er_set_tsid_isdbs(&demod_, static_cast<u16>(params_.stream_id));
}

int XitSqr100Device::Receiver::CheckLock(bool &locked)
{
	std::lock_guard<std::recursive_mutex> lock(parent_.lock_);

	locked = false;
	if (!parent_.available_ || !parent_.receiver_open_ || parent_.terminating_)
		return -ENODEV;

	int ret;
	if (system_ == SystemType::ISDB_T) {
		bool unlocked = false;
		ret = cxd2856er_is_ts_locked_isdbt(&demod_, &locked, &unlocked);
		if (!ret && unlocked)
			return -ECANCELED;
	} else if (system_ == SystemType::ISDB_S)
		ret = cxd2856er_is_ts_locked_isdbs(&demod_, &locked);
	else
		return -EINVAL;

	if (!ret && locked)
		ret = SetCapture(true);
	return ret;
}

int XitSqr100Device::Receiver::SetLnbVoltage(std::int32_t voltage)
{
	std::lock_guard<std::recursive_mutex> lock(parent_.lock_);

	if (!parent_.available_ || !parent_.receiver_open_ || parent_.terminating_)
		return -ENODEV;

	/* XIT-SQR100 はアンテナ給電を備えていない。給電要求は明示的に失敗させる。 */
	return voltage == 0 ? 0 : -ENOSYS;
}

int XitSqr100Device::Receiver::SetCapture(bool capture)
{
	std::lock_guard<std::recursive_mutex> lock(parent_.lock_);

	if (!capture) {
		int ret = streaming_ ? itedtv_bus_stop_streaming(&parent_.bridge_.bus) : 0;
		streaming_ = false;
		parent_.stream_ctx_.remain_len = 0;
		stream_buf_->Stop();
		buffer_started_ = false;
		return ret;
	}

	if (!parent_.available_ || !parent_.receiver_open_ || parent_.terminating_)
		return -ENODEV;

	if (streaming_)
		return 0;

	int ret = it930x_purge_psb(&parent_.bridge_, parent_.purge_timeout_);
	if (ret)
		return ret;

	/* USB 開始に失敗して再試行する場合も、稼働中の配信バッファを再初期化しない。 */
	if (!buffer_started_) {
		const std::size_t bytes = 188 * parent_.buffer_packets_;
		if (!stream_buf_->Alloc(bytes))
			return -ENOMEM;
		stream_buf_->SetThresholdSize(bytes / 10);
		stream_buf_->Start();
		buffer_started_ = true;
	}

	/* USB 開始前に旧キャプチャの端数を破棄し、別の受信へ持ち越さない。 */
	parent_.stream_ctx_.remain_len = 0;

	auto &usb = parent_.bridge_.bus.usb;
	usb.streaming.urb_buffer_size = 188 * parent_.urb_packets_;
	usb.streaming.urb_num = parent_.max_urbs_;
	usb.streaming.no_dma = true;
	usb.streaming.no_raw_io = parent_.no_raw_io_;
	ret = itedtv_bus_start_streaming(&parent_.bridge_.bus, StreamHandler, &parent_);
	if (ret)
		return ret;

	streaming_ = true;

	return 0;
}

int XitSqr100Device::Receiver::ReadStat(command::StatType type, std::int32_t &value)
{
	std::lock_guard<std::recursive_mutex> lock(parent_.lock_);

	if (!parent_.available_ || !parent_.receiver_open_ || parent_.terminating_)
		return -ENODEV;

	value = 0;
	if (type != command::StatType::CNR)
		return -ENOSYS;

	u16 raw;

	int ret;
	if (system_ == SystemType::ISDB_T) {
		ret = cxd2856er_read_cnr_raw_isdbt(&demod_, &raw);
		if (ret)
			return ret;
		if (!raw)
			return -EIO;
		value = static_cast<std::int32_t>(std::log10(raw) * 10000 - 9031);
	} else if (system_ == SystemType::ISDB_S) {
		ret = cxd2856er_read_cnr_raw_isdbs(&demod_, &raw);
		if (ret)
			return ret;
		auto index = std::lower_bound(std::begin(cxd2856er_isdbs_cn_table), std::end(cxd2856er_isdbs_cn_table), raw,
			[](const auto &entry, u16 sample) { return entry.val > sample; });
		if (index == std::end(cxd2856er_isdbs_cn_table))
			--index;
		value = index->cnr;
	} else
		return -EINVAL;

	return 0;
}

} // namespace px4
