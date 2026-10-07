// xit_sqr100_device.hpp
#pragma once

#include <mutex>
#include <condition_variable>
#include <memory>
#include "cxd2856er.h"
#include "cxd6866.h"
#include "device_base.hpp"

namespace px4 {

#define XIT_SQR100_DEVICE_TS_SYNC_COUNT	4
#define XIT_SQR100_DEVICE_TS_SYNC_SIZE	(188 * XIT_SQR100_DEVICE_TS_SYNC_COUNT)

/* 地デジ・衛星で1基の受信機を共用し、カード利用も電源を保持する。 */
class XitSqr100Device final : public DeviceBase {
public:
	XitSqr100Device(const std::wstring &path, const DeviceDefinition &definition,
		std::uintptr_t index, ReceiverManager &receiver_manager);
	~XitSqr100Device() override;
	int Init() override;
	void Term() override;
	void SetAvailability(bool available) override;
	ReceiverBase *GetReceiver(int id) const override;
	bool HasCardReader() const noexcept override { return true; }
	int OpenCard() override;
	void CloseCard() override;
	int DetectCard(bool &detected) override;
	int ResetCard() override;
	int SetCardBaudrate(::it930x_uart_baudrate baudrate) override;
	int IsCardDataReady(bool &ready) override;
	int ReadCardData(std::uint8_t *buf, std::uint8_t &len) override;
	int WriteCardData(const std::uint8_t *buf, std::uint8_t len) override;

private:
	struct StreamContext final {
		/* 終了待機で Receiver を解放する間も、抜去通知と受信コールバックから参照する。 */
		std::shared_ptr<ReceiverBase::StreamBuffer> stream_buf;
		/* USB 入力間の端数を保持し、キャプチャ開始・終了時に破棄する。 */
		std::uint8_t remain_buf[XIT_SQR100_DEVICE_TS_SYNC_SIZE] = {};
		std::size_t remain_len = 0;
	};

	class Receiver final : public ReceiverBase {
	public:
		explicit Receiver(XitSqr100Device &parent);
		~Receiver() override;
		int Open() override;
		void Close() override;
		int CheckLock(bool &locked) override;
		int SetLnbVoltage(std::int32_t voltage) override;
		int SetCapture(bool capture) override;
		int ReadStat(command::StatType type, std::int32_t &value) override;
	protected:
		int SetFrequency() override;
		int SetStreamId() override;
	private:
		XitSqr100Device &parent_;
		std::condition_variable_any close_cond_;
		/* USB 開始に失敗した場合も配信バッファは稼働するため、状態を分ける。 */
		bool streaming_ = false;
		bool buffer_started_ = false;
		SystemType system_ = SystemType::UNSPECIFIED;
		cxd2856er_demod demod_ = {};
		cxd6866_tuner tuner_ = {};
	};
	void LoadConfig();
	int SetBackendPower(bool state);
	static void StreamProcess(std::shared_ptr<ReceiverBase::StreamBuffer> stream_buf, std::uint8_t **buf, std::size_t &len);
	static int StreamHandler(void *context, void *data, std::uint32_t size);

	std::recursive_mutex lock_;
	unsigned int xfer_packets_ = 816, urb_packets_ = 816, max_urbs_ = 5;
	unsigned int buffer_packets_ = 2048;
	int purge_timeout_ = 2000;
	bool no_raw_io_ = false;
	/* 受信機は1基で、開閉とカードの共有電源判定を同じロック下の状態で管理する。 */
	bool receiver_open_ = false;
	std::unique_ptr<Receiver> receiver_;
	bool available_ = true;
	bool initialized_ = false;
	bool terminating_ = false;
	bool card_open_ = false;
	it930x_bridge bridge_ = {};
	StreamContext stream_ctx_;
};

} // namespace px4
