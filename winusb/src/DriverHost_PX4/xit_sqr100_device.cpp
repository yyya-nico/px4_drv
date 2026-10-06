// xit_sqr100_device.cpp
#include "xit_sqr100_device.hpp"
#include <cmath>
#include <iterator>
#include "util.hpp"

namespace px4 {

XitSqr100Device::XitSqr100Device(const std::wstring &path,
	const DeviceDefinition &definition, std::uintptr_t index,
	ReceiverManager &receiver_manager)
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
	/* USB TS 暗号化を bypass し、放送のスクランブルはそのまま公開する。 */
	if (!ret) ret = it930x_write_reg(&bridge_, 0xdab0, 1);
	if (!ret) ret = it930x_write_reg(&bridge_, 0xdaae, 0);
	if (!ret) ret = it930x_set_gpio_mode(&bridge_, 5, IT930X_GPIO_OUT, true);
	if (!ret) ret = it930x_write_gpio(&bridge_, 5, false);
	if (!ret) ret = it930x_set_gpio_mode(&bridge_, 2, IT930X_GPIO_OUT, true);
	if (!ret) ret = it930x_write_gpio(&bridge_, 2, false);
	if (ret) {
		it930x_term(&bridge_);
		itedtv_bus_term(&bridge_.bus);
		return ret;
	}
	receiver_.reset(new Receiver(*this));
	stream_buffer_ = receiver_->GetStreamBuffer();
	initialized_ = true;
	for (const auto &definition : device_def_.receivers) {
		command::ReceiverInfo info = {};
		wcscpy_s(info.device_name, device_def_.name.c_str());
		info.device_guid = device_def_.guid;
		wcscpy_s(info.receiver_name, definition.name.c_str());
		info.receiver_guid = definition.guid;
		info.systems = definition.systems;
		info.index = definition.index;
		info.data_id = 0;
		receiver_manager_.Register(info, receiver_.get());
	}
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
	stream_buffer_.reset();
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
		if (stream_buffer_) stream_buffer_->StopRequest();
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
	if (ret) { SetBackendPower(false); return ret; }
	/* カード初期化の失敗は受信機の登録や受信を中止させない。 */
	ret = it930x_bcas_init_extended(&bridge_);
	/* キャプチャでは H7 出力 Low の後に H1 でカードをリセットする。 */
	if (!ret) ret = it930x_set_gpio_mode(&bridge_, 7, IT930X_GPIO_OUT, true);
	if (!ret)
		ret = it930x_write_gpio(&bridge_, 7, false);
	if (!ret)
		ret = it930x_set_gpio_mode(&bridge_, 1, IT930X_GPIO_OUT, true);
	if (!ret)
		ret = it930x_write_gpio(&bridge_, 1, false);
	if (!ret)
		card_open_ = true;
	else if (!receiver_open_)
		SetBackendPower(false);
	return ret;
}

void XitSqr100Device::CloseCard()
{
	std::lock_guard<std::recursive_mutex> lock(lock_);
	if (!card_open_) return;
	if (available_ && initialized_ && card_open_)
		it930x_write_gpio(&bridge_, 1, false);
	/* H7 の信号の意味は未確定のため、カード側は H1 のリセットで閉じる。 */
	card_open_ = false;
	if (!receiver_open_)
		SetBackendPower(false);
}

int XitSqr100Device::DetectCard(bool &detected)
{
	std::lock_guard<std::recursive_mutex> lock(lock_);
	if (!available_ || !initialized_ || terminating_) return -ENODEV;
	/* カード未接続時の状態監視でも、UART 初期化や電源投入は不要。 */
	int ret = it930x_set_gpio_mode(&bridge_, 15, IT930X_GPIO_IN, true);
	/* H15 はカード挿入時に High となるため、通常機種の検出極性を適用しない。 */
	if (!ret) ret = it930x_read_gpio(&bridge_, 15, &detected);
	return ret;
}

int XitSqr100Device::ResetCard()
{
	std::lock_guard<std::recursive_mutex> lock(lock_);
	return available_ && initialized_ && card_open_ ?
		it930x_bcas_reset_card(&bridge_) : -ENODEV;
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
	if (config.Exists(L"XferPackets")) xfer_packets_ = util::wtoui(config.Get(L"XferPackets"));
	if (config.Exists(L"UrbMaxPackets")) urb_packets_ = util::wtoui(config.Get(L"UrbMaxPackets"));
	if (config.Exists(L"MaxUrbs")) max_urbs_ = util::wtoui(config.Get(L"MaxUrbs"));
	if (config.Exists(L"ReceiverMaxPackets")) buffer_packets_ = util::wtoui(config.Get(L"ReceiverMaxPackets"));
	if (config.Exists(L"PsbPurgeTimeout")) purge_timeout_ = util::wtoi(config.Get(L"PsbPurgeTimeout"));
	if (config.Exists(L"NoRawIo")) no_raw_io_ = util::wtob(config.Get(L"NoRawIo"));
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
		if (ret) return ret;
		Sleep(50);
		ret = it930x_write_gpio(&bridge_, 2, false);
		if (ret) return ret;
		Sleep(100);
		ret = it930x_write_gpio(&bridge_, 2, true);
		if (ret) return ret;
		Sleep(20);
	} else {
		/* Linux の停止手順と同じ順序で両方の停止を試みる。 */
		int ret = it930x_write_gpio(&bridge_, 5, false);
		int ret2 = it930x_write_gpio(&bridge_, 2, false);
		return ret ? ret : ret2;
	}
	return 0;
}

int XitSqr100Device::StreamHandler(void *context, void *data, std::uint32_t size)
{
	auto &device = *static_cast<XitSqr100Device *>(context);
	device.framer_.Feed(static_cast<const std::uint8_t *>(data), size,
		[&](const std::uint8_t *packets, std::size_t bytes) {
			device.stream_buffer_->Write(packets, bytes);
		});
	device.stream_buffer_->NotifyWrite();
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
	close_cond_.wait(lock, [this] { return !open_; });
}

int XitSqr100Device::Receiver::Open()
{
	std::lock_guard<std::recursive_mutex> lock(parent_.lock_);
	if (!parent_.available_ || !parent_.initialized_ || parent_.terminating_) return -ENODEV;
	if (open_) return -EALREADY;
	int ret = parent_.card_open_ ? 0 : parent_.SetBackendPower(true);
	if (ret) { parent_.SetBackendPower(false); return ret; }
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
	if (!ret) ret = cxd2856er_write_slvx_reg(&demod_, 0x08, 0);
	if (!ret) ret = cxd2856er_write_slvt_reg(&demod_, 0x00, 0);
	if (!ret) ret = cxd2856er_write_slvt_reg_mask(&demod_, 0xc4, 0x80, 0x88);
	if (!ret) ret = cxd2856er_write_slvt_reg(&demod_, 0x00, 0xa0);
	if (!ret) ret = cxd2856er_write_slvt_reg_mask(&demod_, 0xb9, 1, 1);
	if (ret) {
		if (tuner_initialized) cxd6866_term(&tuner_);
		if (demod_initialized) cxd2856er_term(&demod_);
		if (!parent_.card_open_) parent_.SetBackendPower(false);
		return ret;
	}
	parent_.receiver_open_ = true;
	open_ = true;
	return 0;
}

void XitSqr100Device::Receiver::Close()
{
	std::unique_lock<std::recursive_mutex> lock(parent_.lock_);
	if (!open_) return;
	SetCapture(false);
	if (parent_.available_) {
		cxd6866_term(&tuner_);
		cxd2856er_term(&demod_);
	}
	parent_.receiver_open_ = false;
	if (!parent_.card_open_) parent_.SetBackendPower(false);
	open_ = false;
	system_ = SystemType::UNSPECIFIED;
	lock.unlock();
	close_cond_.notify_all();
}

int XitSqr100Device::Receiver::SetFrequency()
{
	std::lock_guard<std::recursive_mutex> lock(parent_.lock_);
	if (!parent_.available_ || !open_ || parent_.terminating_) return -ENODEV;
	/* 選局前に USB を回収し、旧チャンネルの TS と端数を破棄する。 */
	int ret = PauseCapture();
	if (ret) return ret;
	system_ = SystemType::UNSPECIFIED;
	cxd2856er_system_params parameters = {};
	if (params_.system == SystemType::ISDB_T) {
		parameters.bandwidth = params_.bandwidth ? params_.bandwidth : 6;
		ret = cxd2856er_wakeup(&demod_, CXD2856ER_ISDB_T_SYSTEM, &parameters);
		if (!ret) ret = cxd6866_set_params_t(&tuner_, CXD6866_ISDB_T_SYSTEM, params_.freq, 6);
	} else if (params_.system == SystemType::ISDB_S) {
		ret = cxd2856er_wakeup(&demod_, CXD2856ER_ISDB_S_SYSTEM, &parameters);
		if (!ret) ret = cxd6866_set_params_s(&tuner_, CXD6866_ISDB_S_SYSTEM, params_.freq, 28860);
	} else return -EINVAL;
	if (!ret) ret = cxd2856er_post_tune(&demod_);
	if (!ret) system_ = params_.system;
	return ret;
}

int XitSqr100Device::Receiver::SetStreamId()
{
	std::lock_guard<std::recursive_mutex> lock(parent_.lock_);
	if (!parent_.available_ || !open_ || parent_.terminating_) return -ENODEV;
	if (params_.system != SystemType::ISDB_S || params_.stream_id > UINT16_MAX) return -EINVAL;
	int ret = PauseCapture();
	if (ret) return ret;
	return params_.stream_id < 12 ? cxd2856er_set_slot_isdbs(&demod_, static_cast<u16>(params_.stream_id)) :
		cxd2856er_set_tsid_isdbs(&demod_, static_cast<u16>(params_.stream_id));
}

int XitSqr100Device::Receiver::CheckLock(bool &locked)
{
	std::lock_guard<std::recursive_mutex> lock(parent_.lock_);
	locked = false;
	if (!parent_.available_ || !open_ || parent_.terminating_) return -ENODEV;
	int ret;
	if (system_ == SystemType::ISDB_T) {
		bool unlocked = false;
		ret = cxd2856er_is_ts_locked_isdbt(&demod_, &locked, &unlocked);
		if (!ret && unlocked) return -ECANCELED;
	} else if (system_ == SystemType::ISDB_S)
		ret = cxd2856er_is_ts_locked_isdbs(&demod_, &locked);
	else return -EINVAL;
	if (!ret && locked) ret = SetCapture(true);
	return ret;
}

int XitSqr100Device::Receiver::SetLnbVoltage(std::int32_t voltage)
{
	std::lock_guard<std::recursive_mutex> lock(parent_.lock_);
	if (!parent_.available_ || !open_ || parent_.terminating_) return -ENODEV;
	/* XIT-SQR100 はアンテナ給電を備えていない。給電要求は明示的に失敗させる。 */
	return voltage == 0 ? 0 : -ENOSYS;
}

int XitSqr100Device::Receiver::PauseCapture()
{
	/* 選局中もパイプ配信スレッドを維持するため、StreamBuffer::Stop は終了時だけ使う。 */
	int ret = streaming_ ? itedtv_bus_stop_streaming(&parent_.bridge_.bus) : 0;
	streaming_ = false;
	if (!stream_buf_->Purge() && !ret) ret = -EIO;
	parent_.framer_.Reset();
	return ret;
}

int XitSqr100Device::Receiver::SetCapture(bool capture)
{
	std::lock_guard<std::recursive_mutex> lock(parent_.lock_);
	if (!capture) {
		int ret = PauseCapture();
		stream_buf_->Stop();
		buffer_started_ = false;
		return ret;
	}
	if (!parent_.available_ || !open_ || parent_.terminating_) return -ENODEV;
	if (streaming_) return 0;
	int ret = it930x_purge_psb(&parent_.bridge_, parent_.purge_timeout_);
	if (ret) return ret;
	/* 選局時はバッファを停止しないため、確保と開始は最初のキャプチャに限る。 */
	if (!buffer_started_) {
		const std::size_t bytes = 188 * parent_.buffer_packets_;
		if (!stream_buf_->Alloc(bytes)) return -ENOMEM;
		stream_buf_->SetThresholdSize(bytes / 10);
		stream_buf_->Start();
		buffer_started_ = true;
	}
	parent_.framer_.Reset();
	auto &usb = parent_.bridge_.bus.usb;
	usb.streaming.urb_buffer_size = 188 * parent_.urb_packets_;
	usb.streaming.urb_num = parent_.max_urbs_;
	usb.streaming.no_dma = true;
	usb.streaming.no_raw_io = parent_.no_raw_io_;
	ret = itedtv_bus_start_streaming(&parent_.bridge_.bus, StreamHandler, &parent_);
	if (ret) return ret;
	streaming_ = true;
	return 0;
}

/* CXD2856ER の衛星 CNR は既存 PXMLT と同じ dB * 1000 の表で返す。 */
static const struct { u16 val; std::int32_t cn; } isdbs_cn_table[] = {
	{ 0x5af, 0 }, { 0x597, 100 }, { 0x57e, 200 }, { 0x567, 300 },
	{ 0x550, 400 }, { 0x539, 500 }, { 0x522, 600 }, { 0x50c, 700 },
	{ 0x4f6, 800 }, { 0x4e1, 900 }, { 0x4cc, 1000 }, { 0x4b6, 1100 },
	{ 0x4a1, 1200 }, { 0x48c, 1300 }, { 0x477, 1400 }, { 0x463, 1500 },
	{ 0x44f, 1600 }, { 0x43c, 1700 }, { 0x428, 1800 }, { 0x416, 1900 },
	{ 0x403, 2000 }, { 0x3ef, 2100 }, { 0x3dc, 2200 }, { 0x3c9, 2300 },
	{ 0x3b6, 2400 }, { 0x3a4, 2500 }, { 0x392, 2600 }, { 0x381, 2700 },
	{ 0x36f, 2800 }, { 0x35f, 2900 }, { 0x34e, 3000 }, { 0x33d, 3100 },
	{ 0x32d, 3200 }, { 0x31d, 3300 }, { 0x30d, 3400 }, { 0x2fd, 3500 },
	{ 0x2ee, 3600 }, { 0x2df, 3700 }, { 0x2d0, 3800 }, { 0x2c2, 3900 },
	{ 0x2b4, 4000 }, { 0x2a6, 4100 }, { 0x299, 4200 }, { 0x28c, 4300 },
	{ 0x27f, 4400 }, { 0x272, 4500 }, { 0x265, 4600 }, { 0x259, 4700 },
	{ 0x24d, 4800 }, { 0x241, 4900 }, { 0x236, 5000 }, { 0x22b, 5100 },
	{ 0x220, 5200 }, { 0x215, 5300 }, { 0x20a, 5400 }, { 0x200, 5500 },
	{ 0x1f6, 5600 }, { 0x1ec, 5700 }, { 0x1e2, 5800 }, { 0x1d8, 5900 },
	{ 0x1cf, 6000 }, { 0x1c6, 6100 }, { 0x1bc, 6200 }, { 0x1b3, 6300 },
	{ 0x1aa, 6400 }, { 0x1a2, 6500 }, { 0x199, 6600 }, { 0x191, 6700 },
	{ 0x189, 6800 }, { 0x181, 6900 }, { 0x179, 7000 }, { 0x171, 7100 },
	{ 0x169, 7200 }, { 0x161, 7300 }, { 0x15a, 7400 }, { 0x153, 7500 },
	{ 0x14b, 7600 }, { 0x144, 7700 }, { 0x13d, 7800 }, { 0x137, 7900 },
	{ 0x130, 8000 }, { 0x12a, 8100 }, { 0x124, 8200 }, { 0x11e, 8300 },
	{ 0x118, 8400 }, { 0x112, 8500 }, { 0x10c, 8600 }, { 0x107, 8700 },
	{ 0x101, 8800 }, { 0xfc, 8900 }, { 0xf7, 9000 }, { 0xf2, 9100 },
	{ 0xec, 9200 }, { 0xe7, 9300 }, { 0xe2, 9400 }, { 0xdd, 9500 },
	{ 0xd8, 9600 }, { 0xd4, 9700 }, { 0xcf, 9800 }, { 0xca, 9900 },
	{ 0xc6, 10000 }, { 0xc2, 10100 }, { 0xbe, 10200 }, { 0xb9, 10300 },
	{ 0xb5, 10400 }, { 0xb1, 10500 }, { 0xae, 10600 }, { 0xaa, 10700 },
	{ 0xa6, 10800 }, { 0xa3, 10900 }, { 0x9f, 11000 }, { 0x9b, 11100 },
	{ 0x98, 11200 }, { 0x95, 11300 }, { 0x91, 11400 }, { 0x8e, 11500 },
	{ 0x8b, 11600 }, { 0x88, 11700 }, { 0x85, 11800 }, { 0x82, 11900 },
	{ 0x7f, 12000 }, { 0x7c, 12100 }, { 0x7a, 12200 }, { 0x77, 12300 },
	{ 0x74, 12400 }, { 0x72, 12500 }, { 0x6f, 12600 }, { 0x6d, 12700 },
	{ 0x6b, 12800 }, { 0x68, 12900 }, { 0x66, 13000 }, { 0x64, 13100 },
	{ 0x61, 13200 }, { 0x5f, 13300 }, { 0x5d, 13400 }, { 0x5b, 13500 },
	{ 0x59, 13600 }, { 0x57, 13700 }, { 0x55, 13800 }, { 0x53, 13900 },
	{ 0x51, 14000 }, { 0x4f, 14100 }, { 0x4e, 14200 }, { 0x4c, 14300 },
	{ 0x4a, 14400 }, { 0x49, 14500 }, { 0x47, 14600 }, { 0x45, 14700 },
	{ 0x44, 14800 }, { 0x42, 14900 }, { 0x41, 15000 }, { 0x3f, 15100 },
	{ 0x3e, 15200 }, { 0x3c, 15300 }, { 0x3b, 15400 }, { 0x3a, 15500 },
	{ 0x38, 15600 }, { 0x37, 15700 }, { 0x36, 15800 }, { 0x34, 15900 },
	{ 0x33, 16000 }, { 0x32, 16100 }, { 0x31, 16200 }, { 0x30, 16300 },
	{ 0x2f, 16400 }, { 0x2e, 16500 }, { 0x2d, 16600 }, { 0x2c, 16700 },
	{ 0x2b, 16800 }, { 0x2a, 16900 }, { 0x29, 17000 }, { 0x28, 17100 },
	{ 0x27, 17200 }, { 0x26, 17300 }, { 0x25, 17400 }, { 0x24, 17500 },
	{ 0x23, 17600 }, { 0x22, 17800 }, { 0x21, 17900 }, { 0x20, 18000 },
	{ 0x1f, 18200 }, { 0x1e, 18300 }, { 0x1d, 18500 }, { 0x1c, 18700 },
	{ 0x1b, 18900 }, { 0x1a, 19000 }, { 0x19, 19200 }, { 0x18, 19300 },
	{ 0x17, 19500 }, { 0x16, 19700 }, { 0x15, 19900 }, { 0x14, 20000 }
};

int XitSqr100Device::Receiver::ReadStat(command::StatType type, std::int32_t &value)
{
	std::lock_guard<std::recursive_mutex> lock(parent_.lock_);
	if (!parent_.available_ || !open_ || parent_.terminating_) return -ENODEV;
	value = 0;
	if (type != command::StatType::CNR) return -ENOSYS;
	u16 raw;
	int ret;
	if (system_ == SystemType::ISDB_T) {
		ret = cxd2856er_read_cnr_raw_isdbt(&demod_, &raw);
		if (ret) return ret;
		if (!raw) return -EIO;
		value = static_cast<std::int32_t>(std::log10(raw) * 10000 - 9031);
	} else if (system_ == SystemType::ISDB_S) {
		ret = cxd2856er_read_cnr_raw_isdbs(&demod_, &raw);
		if (ret) return ret;
		auto index = std::lower_bound(std::begin(isdbs_cn_table), std::end(isdbs_cn_table), raw,
			[](const auto &entry, u16 sample) { return entry.val > sample; });
		if (index == std::end(isdbs_cn_table)) --index;
		value = index->cn;
	} else return -EINVAL;
	return 0;
}

} // namespace px4
