// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace uml::runtime {

/* Formats into a fixed buffer; what does not fit is dropped. */
class FixedText final {
public:
	FixedText(char *data, std::size_t size) noexcept : data_{data}, size_{size} {}

	FixedText &text(std::string_view value) noexcept
	{
		const std::size_t count = std::min(value.size(), size_ - used_);
		std::memcpy(data_ + used_, value.data(), count);
		used_ += count;
		return *this;
	}

	FixedText &number(std::int64_t value) noexcept
	{
		char digits[24];
		const auto result =
			std::to_chars(digits, digits + sizeof(digits), value);
		return text({digits, static_cast<std::size_t>(result.ptr - digits)});
	}

	/* What `printable` stores of `character`: a carriage return is dropped
	 * (the only byte of which nothing stays), every other byte that is not
	 * printable ASCII, or a line feed or tab where `lines` allows one, is
	 * stored as `?`. A reader of the stored text sees these bytes and no
	 * others, so whatever must not be stored is matched on them
	 * (`printable_text`). */
	[[nodiscard]] static constexpr bool dropped(char character) noexcept
	{
		return character == '\r';
	}

	[[nodiscard]] static constexpr char stored(char character, bool lines)
		noexcept
	{
		const bool keep = (character >= ' ' && character < 0x7f) ||
			(lines && (character == '\n' || character == '\t'));
		return keep ? character : '?';
	}

	/* `value` with every byte that is not printable ASCII, or a line feed
	 * where `lines` allows one, replaced; carriage returns are dropped. */
	FixedText &printable(std::string_view value, bool lines) noexcept
	{
		for (const char character : value) {
			if (used_ == size_)
				break;
			if (dropped(character))
				continue;
			data_[used_++] = stored(character, lines);
		}
		return *this;
	}

	/* `value` as `printable` would store it, in `scratch` as far as that
	 * reaches: the text a mask has to be matched on. */
	[[nodiscard]] static std::string_view printable_text(char *scratch,
		std::size_t size, std::string_view value, bool lines) noexcept
	{
		std::size_t used = 0;
		for (const char character : value) {
			if (used == size)
				break;
			if (dropped(character))
				continue;
			scratch[used++] = stored(character, lines);
		}
		return {scratch, used};
	}

	[[nodiscard]] std::size_t size() const noexcept
	{
		return used_;
	}

private:
	char *data_;
	std::size_t size_;
	std::size_t used_{};
};

} // namespace uml::runtime
