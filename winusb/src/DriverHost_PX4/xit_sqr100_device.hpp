// xit_sqr100_device.hpp
#pragma once

#include <mutex>
#include <condition_variable>
#include <memory>
#include "cxd2856er.h"
#include "cxd6866.h"
#include "xit_ts_framer.hpp"
#include "device_base.hpp"

namespace px4 {

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
		bool open_ = false;
		bool streaming_ = false;
		SystemType system_ = SystemType::UNSPECIFIED;
		cxd2856er_demod demod_ = {};
		cxd6866_tuner tuner_ = {};
	};
	void LoadConfig();
	int SetBackendPower(bool state);
	static int StreamHandler(void *context, void *data, std::uint32_t size);

	std::recursive_mutex lock_;
	unsigned int xfer_packets_ = 816, urb_packets_ = 816, max_urbs_ = 5;
	unsigned int buffer_packets_ = 2048;
	int purge_timeout_ = 2000;
	bool no_raw_io_ = false;
	bool receiver_open_ = false;
	std::unique_ptr<Receiver> receiver_;
	std::shared_ptr<ReceiverBase::StreamBuffer> stream_buffer_;
	XitTsFramer framer_;
	bool available_ = true;
	bool initialized_ = false;
	bool terminating_ = false;
	bool card_open_ = false;
	it930x_bridge bridge_ = {};
};

} // namespace px4
