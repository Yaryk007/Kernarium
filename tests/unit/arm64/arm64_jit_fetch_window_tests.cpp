// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/jit/jit_fetch_window.hpp"
#include "require.hpp"

#include <array>
#include <cstring>

using uml::testing::require;

int main() {
  constexpr std::uint32_t nop = 0xd503201fU;
  constexpr std::uint32_t branch = 0x14000000U;
  constexpr std::uint64_t page = 4096;
  std::array<std::uint32_t, 64> source{}, result{};
  std::size_t calls = 0;
  auto read = [&](std::uint64_t pc, std::size_t readable) {
    calls = 0;
    return uml::cpu::arm64::fetch_a64_jit_window(pc, result, page,
        [&](std::uint64_t address, std::span<std::byte> bytes) {
          ++calls;
          require(address / page == (address + bytes.size() - 1) / page,
                  "a fetch crossed a page");
          auto offset = static_cast<std::size_t>(address - pc);
          if (offset + bytes.size() > readable * sizeof(source[0]))
            return false;
          std::memcpy(bytes.data(), reinterpret_cast<const std::byte *>(source.data()) + offset,
                      bytes.size());
          return true;
        });
  };

  source.fill(nop);
  require(read(page, 64) == 64 && calls == 8 && result == source, "batch straight-line code");
  for (std::size_t length = 0; length < 64; ++length) {
    source.fill(nop);
    require(read(page - 4, length) == length, "preserve the readable prefix");
    source[length] = branch;
    require(read(page - 4, length + 1) == length + 1, "stop at the branch");
    require(std::equal(result.begin(), result.begin() + length + 1, source.begin()),
            "preserve instructions up to the branch");
    if (length == 0)
      require(calls == 1, "branch on the last word never probes the next page");
  }
  source[0] = 0x08400000U;
  source[1] = 0x6b00001fU;
  source[2] = 0x54000001U;
  for (std::size_t readable = 3; readable <= 6; ++readable)
    require(read(page - 12, readable) == std::min<std::size_t>(5, readable),
            "keep bounded compare/exchange lookahead");
}
