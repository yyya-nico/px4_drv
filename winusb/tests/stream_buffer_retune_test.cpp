#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include "receiver_base.hpp"

int main()
{
	px4::ReceiverBase::StreamBuffer buffer;
	if (!buffer.Alloc(188 * 16)) return 1;
	buffer.SetThresholdSize(188);
	buffer.Start();
	std::atomic<unsigned int> received{0};
	std::atomic_bool exited{false};
	std::atomic_bool invalid{false};
	std::thread reader([&] {
		buffer.HandleRead(188, [&](const void *data, std::size_t size) {
			const auto *bytes = static_cast<const unsigned char *>(data);
			if (size != 188 || bytes[0] != 0x47 || bytes[1] != received.load())
				invalid = true;
			++received;
			return true;
		});
		exited = true;
	});
	/* 選局中の旧データ破棄で配信が終了せず、新しい TS を同じスレッドへ渡せることを確認する。 */
	for (unsigned int cycle = 0; cycle < 20; ++cycle) {
		if (!buffer.Purge()) std::_Exit(1);
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		if (exited) std::_Exit(1);
		std::array<unsigned char, 188> packet{};
		packet[0] = 0x47;
		packet[1] = static_cast<unsigned char>(cycle);
		auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		std::size_t size = packet.size();
		if (!buffer.Write(packet.data(), size) || size != packet.size()) std::_Exit(1);
		while (received != cycle + 1 && std::chrono::steady_clock::now() < deadline) {
			buffer.NotifyWrite();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		if (received != cycle + 1 || invalid) std::_Exit(1);
	}
	buffer.StopRequest();
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!exited && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	if (!exited) std::_Exit(1);
	reader.join();
	buffer.Stop();
	std::puts("stream_buffer_retune_test: passed (20 pauses, reader stopped)");
	return 0;
}
