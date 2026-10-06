#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
#include "xit_ts_framer.hpp"

static std::vector<std::uint8_t> Packets(unsigned int count, std::uint8_t label)
{
	std::vector<std::uint8_t> result(count * 188, label);
	for (unsigned int i = 0; i < count; ++i) result[i * 188] = 0x47;
	return result;
}

int main()
{
	const auto packets = Packets(16, 0x12);
	/* あらゆる1回の分割位置で順序・端数・最後のパケットを保持する。 */
	for (std::size_t split = 0; split <= packets.size(); ++split) {
		px4::XitTsFramer framer;
		std::vector<std::uint8_t> output;
		auto write = [&](const std::uint8_t *data, std::size_t size) {
			assert(size % 188 == 0);
			output.insert(output.end(), data, data + size);
		};
		framer.Feed(packets.data(), split, write);
		framer.Feed(packets.data() + split, packets.size() - split, write);
		assert(output == packets);
	}
	px4::XitTsFramer framer;
	std::vector<std::uint8_t> output;
	auto write = [&](const std::uint8_t *data, std::size_t size) {
		output.insert(output.end(), data, data + size);
	};
	/* ノイズと1 byte入力から再同期する。 */
	const std::uint8_t noise[] = {0x00, 0x47, 0x12};
	framer.Feed(noise, sizeof(noise), write);
	for (auto byte : packets) framer.Feed(&byte, 1, write);
	assert(output == packets);
	/* 同期済みの途中へノイズが入っても後続の境界を回復する。 */
	output.clear();
	framer.Feed(noise, sizeof(noise), write);
	framer.Feed(packets.data(), packets.size(), write);
	assert(output == packets);
	/* 選局時の Reset で前チャンネルの端数を次へ混ぜない。 */
	framer.Feed(packets.data(), 83, write);
	framer.Reset(); output.clear();
	const auto next = Packets(8, 0x34);
	framer.Feed(next.data(), next.size(), write);
	assert(output == next);
	puts("XIT TS framer test: passed");
}
