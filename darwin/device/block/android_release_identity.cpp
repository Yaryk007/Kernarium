// SPDX-License-Identifier: GPL-2.0-or-later

#include "android_release_identity.hpp"

#include <CommonCrypto/CommonDigest.h>

#include <cctype>
#include <cstdio>
#include <cstdint>
#include <string_view>
#include <vector>

#include <sys/stat.h>

namespace uml::android {
namespace {

constexpr std::uint64_t maximumManifestBytes = 4U * mebibyte;
constexpr std::size_t maximumDepth = 64;
constexpr std::string_view manifestName{"release-manifest.json"};

/* Walks a JSON document without keeping it: the members of the top-level
 * object are read one by one, the ones that are not wanted skipped by their
 * brackets and quotes alone. */
class Scanner final {
public:
	explicit Scanner(std::string_view text) : text_(text) {}

	std::optional<std::string> member(std::string_view key)
	{
		skipSpace();
		if (!take('{'))
			return std::nullopt;
		std::optional<std::string> found;
		skipSpace();
		if (take('}'))
			return finish(found);
		for (;;) {
			skipSpace();
			std::string_view name;
			if (!string(name))
				return std::nullopt;
			skipSpace();
			if (!take(':'))
				return std::nullopt;
			skipSpace();
			if (position_ < text_.size() && text_[position_] == '"') {
				std::string_view value;
				if (!string(value))
					return std::nullopt;
				if (name == key) {
					if (found || value.find('\\') != std::string_view::npos)
						return std::nullopt;
					found = std::string{value};
				}
			} else if (!skipValue(0)) {
				return std::nullopt;
			}
			skipSpace();
			if (take(','))
				continue;
			if (!take('}'))
				return std::nullopt;
			return finish(found);
		}
	}

private:
	std::optional<std::string> finish(std::optional<std::string> found)
	{
		skipSpace();
		if (position_ != text_.size())
			return std::nullopt;
		return found;
	}

	void skipSpace()
	{
		while (position_ < text_.size() &&
		       (text_[position_] == ' ' || text_[position_] == '\n' ||
			text_[position_] == '\r' || text_[position_] == '\t'))
			++position_;
	}

	bool take(char character)
	{
		if (position_ < text_.size() && text_[position_] == character) {
			++position_;
			return true;
		}
		return false;
	}

	/* A string's raw text, quotes excluded; the escapes stay as written. */
	bool string(std::string_view &value)
	{
		if (!take('"'))
			return false;
		const auto begin = position_;
		while (position_ < text_.size()) {
			const char character = text_[position_];
			if (character == '"') {
				value = text_.substr(begin, position_ - begin);
				++position_;
				return true;
			}
			if (static_cast<unsigned char>(character) < 0x20U)
				return false;
			position_ += character == '\\' ? 2U : 1U;
		}
		return false;
	}

	bool skipValue(std::size_t depth)
	{
		if (depth > maximumDepth || position_ >= text_.size())
			return false;
		const char first = text_[position_];
		if (first == '"') {
			std::string_view ignored;
			return string(ignored);
		}
		if (first == '{' || first == '[') {
			const char close = first == '{' ? '}' : ']';
			++position_;
			skipSpace();
			if (take(close))
				return true;
			for (;;) {
				skipSpace();
				if (first == '{') {
					std::string_view ignored;
					if (!string(ignored))
						return false;
					skipSpace();
					if (!take(':'))
						return false;
					skipSpace();
				}
				if (!skipValue(depth + 1U))
					return false;
				skipSpace();
				if (take(','))
					continue;
				return take(close);
			}
		}
		/* A number, true, false or null: it runs to the next delimiter. */
		const auto begin = position_;
		while (position_ < text_.size() && text_[position_] != ',' &&
		       text_[position_] != '}' && text_[position_] != ']' &&
		       text_[position_] != ' ' && text_[position_] != '\n' &&
		       text_[position_] != '\r' && text_[position_] != '\t')
			++position_;
		return position_ != begin;
	}

	std::string_view text_;
	std::size_t position_{};
};

std::string hex(const unsigned char *bytes, std::size_t count)
{
	static constexpr char digits[] = "0123456789abcdef";
	std::string text;
	text.reserve(2 * count);
	for (std::size_t index = 0; index < count; ++index) {
		text.push_back(digits[bytes[index] >> 4U]);
		text.push_back(digits[bytes[index] & 15U]);
	}
	return text;
}

/* A release ID as tools/aot/release_artifact.py takes one. */
bool validId(std::string_view id)
{
	if (id.empty() || id.size() > 64)
		return false;
	for (std::size_t index = 0; index < id.size(); ++index) {
		const char character = id[index];
		const bool lower = character >= 'a' && character <= 'z';
		const bool digit = character >= '0' && character <= '9';
		const bool inner = character == '.' || character == '_' ||
			character == '-';
		if (!(lower || digit || (index != 0 && inner)))
			return false;
	}
	return true;
}

bool validDigest(std::string_view digest)
{
	if (digest.size() != 2 * CC_SHA256_DIGEST_LENGTH)
		return false;
	for (const char character : digest)
		if (!((character >= '0' && character <= '9') ||
		      (character >= 'a' && character <= 'f')))
			return false;
	return true;
}

} // namespace

std::optional<std::string> topLevelString(std::string_view text,
					  std::string_view key) noexcept
{
	try {
		return Scanner{text}.member(key);
	} catch (...) {
		return std::nullopt;
	}
}

std::optional<ReleaseIdentity> readReleaseIdentity(
	const std::filesystem::path &release, const Layout &layout) noexcept
{
	try {
		const auto path = release / manifestName;
		struct stat details {};
		if (lstat(path.c_str(), &details) != 0 || !S_ISREG(details.st_mode) ||
		    details.st_size <= 0 ||
		    static_cast<std::uint64_t>(details.st_size) > maximumManifestBytes)
			return std::nullopt;
		std::vector<char> bytes(static_cast<std::size_t>(details.st_size));
		std::FILE *file = std::fopen(path.c_str(), "rb");
		if (file == nullptr)
			return std::nullopt;
		const auto count = std::fread(bytes.data(), 1, bytes.size(), file);
		const int closed = std::fclose(file);
		if (count != bytes.size() || closed != 0)
			return std::nullopt;
		auto id = topLevelString(
			std::string_view{bytes.data(), bytes.size()}, "release_id");
		if (!id || !validId(*id))
			return std::nullopt;
		unsigned char digest[CC_SHA256_DIGEST_LENGTH];
		CC_SHA256(bytes.data(), static_cast<CC_LONG>(bytes.size()), digest);
		const auto &system = layout.systemPartition("system");
		if (!validDigest(system.sha256))
			return std::nullopt;
		return ReleaseIdentity{std::move(*id),
				       hex(digest, sizeof(digest)),
				       system.sha256};
	} catch (...) {
		return std::nullopt;
	}
}

} // namespace uml::android
