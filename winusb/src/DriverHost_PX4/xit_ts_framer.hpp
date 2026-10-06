#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "ts_sync.h"

namespace px4 {
/* コールバック間の端数を順序どおり保持する。選局時は USB 停止後に Reset を呼ぶ。 */
class XitTsFramer final {
public:
	void Reset() noexcept { size_ = 0; synced_ = false; }
	template<class Write>
	void Feed(const std::uint8_t *data, std::size_t length, Write write)
	{
		while (length) {
			if (synced_ && !size_ && length >= 188) {
				std::size_t bytes = 0;
				while (length - bytes >= 188 && px4_ts_has_plain_sync(data[bytes]))
					bytes += 188;
				if (bytes) {
					write(data, bytes); data += bytes; length -= bytes;
					continue;
				}
				synced_ = false;
			}
			const std::size_t target = synced_ ? 188 : sizeof(pending_);
			const auto count = (std::min)(length, target - size_);
			std::memcpy(pending_ + size_, data, count);
			size_ += count; data += count; length -= count;
			if (size_ < target)
				continue;
			bool valid = true;
			for (std::size_t i = 0; i < target; i += 188)
				valid = valid && px4_ts_has_plain_sync(pending_[i]);
			if (valid) {
				write(pending_, target); size_ = 0; synced_ = true;
			} else {
				/* 1 byte ずつ境界を探す間も、次の入力へ端数を残す。 */
				std::memmove(pending_, pending_ + 1, --size_);
				synced_ = false;
			}
		}
	}
private:
	std::uint8_t pending_[188 * 4] = {};
	std::size_t size_ = 0;
	bool synced_ = false;
};
} // namespace px4
