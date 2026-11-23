// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * The virtual Wi-Fi's hardware address, derived from eth0's.
 *
 * The host gives every installation its own eth0 address
 * (darwin/device/block/android_identity.hpp). The guest's Wi-Fi address is
 * a function of it, so installations differ in both and no new value travels
 * on the kernel command line: an `androidboot.*` argument becomes a `ro.boot.*`
 * property that every application can read.
 *
 * The function is a permutation, not a hash: distinct eth0 addresses give
 * distinct Wi-Fi addresses by construction. The host's addresses are locally
 * administered unicast, which leaves 46 free bits; a keyed Feistel network
 * rho (six rounds, halves of 23 bits, the finalizer of MurmurHash3 as the round
 * function) permutes them, and the Wi-Fi address is
 *
 *   rho^-1(rho(eth0) + k)   for the smallest k >= 1 that is not reserved,
 *
 * as a locally administered unicast address again. rho^-1(rho(x) + 1) is a
 * permutation of the 2^46 values with no fixed point and one cycle, so the
 * Wi-Fi address is never eth0's own (k stays far below 2^46), and walking the
 * cycle past a reserved address does not merge two inputs: the map from the
 * unreserved values to the unreserved values is a cyclic shift of the cycle.
 * The reserved addresses are 02:00:00:00:00:00 (what Android shows applications
 * that may not see the real one) and the host's virtual router and name
 * server, 52:55:0a:00:02:xx (darwin/device/network/slirp/provider.hpp): 257 of
 * them, so k is at most 258. The function is invertible (EthernetFromWifiMac),
 * which the tests use to show it is one to one.
 *
 * What the host never generates gets a result and no guarantee. The flag
 * bits of the first octet are ignored (only 46 bits are permuted), so an
 * address that is not locally administered unicast, such as the broadcast
 * address, gives the result of its locally administered unicast twin; and a
 * reserved input (the 257 addresses above, which the host refuses) gives the
 * result of the unreserved address before it on the cycle. Everything else
 * is distinct from everything else.
 *
 * tests/unit/andarium/wifi_mac_tests.cpp runs this header against the golden
 * vectors that tests/integration/android/run_android_installation_identity.py
 * checks too, where the Python port is the runner's model of the guest.
 */

#ifndef KERNARIUM_WIFI_MAC_H_
#define KERNARIUM_WIFI_MAC_H_

#include <stddef.h>
#include <stdint.h>

#include <array>

namespace kernarium_net {

using Mac = std::array<unsigned char, 6>;

// "krn_wifi".
constexpr uint64_t kWifiMacKey = 0x6b726e5f77696669ULL;
// The free bits of a locally administered unicast address, and the halves of
// the Feistel network over them.
constexpr unsigned kWifiMacBits = 46;
constexpr unsigned kWifiMacHalfBits = 23;
constexpr uint64_t kWifiMacHalfMask = (1ULL << kWifiMacHalfBits) - 1;
constexpr uint64_t kWifiMacValues = 1ULL << kWifiMacBits;
constexpr unsigned kWifiMacRounds = 6;
// The reserved addresses (02:00:00:00:00:00 and 52:55:0a:00:02:00 to :ff) bound
// the walk along the cycle.
constexpr uint64_t kWifiMacMaxSteps = 258;

// The finalizer of MurmurHash3's 64-bit hash: a bijection.
constexpr uint64_t Mix64(uint64_t value) {
  value ^= value >> 33;
  value *= 0xff51afd7ed558ccdULL;
  value ^= value >> 33;
  value *= 0xc4ceb9fe1a85ec53ULL;
  value ^= value >> 33;
  return value;
}

constexpr uint64_t PackMac(const Mac& mac) {
  uint64_t packed = 0;
  for (size_t index = 0; index < mac.size(); ++index) {
    packed = packed << 8 | mac[index];
  }
  return packed;
}

// An address without its two flag bits: the first octet's upper six bits and
// the five octets after it.
constexpr uint64_t WifiMacBits(const Mac& mac) {
  return static_cast<uint64_t>(mac[0] >> 2) << 40 | (PackMac(mac) & 0xffffffffffULL);
}

// The locally administered unicast address of 46 bits.
constexpr Mac WifiMacFromBits(uint64_t bits) {
  Mac mac = {};
  mac[0] = static_cast<unsigned char>((bits >> 40 << 2) | 0x02);
  for (size_t index = 1; index < mac.size(); ++index) {
    mac[index] = static_cast<unsigned char>(bits >> (8 * (5 - index)));
  }
  return mac;
}

// Whether an address is one that no guest may take as its Wi-Fi address.
constexpr bool WifiMacReserved(const Mac& mac) {
  const uint64_t packed = PackMac(mac);
  return packed == 0x020000000000ULL || (packed >> 8) == 0x52550a0002ULL;
}

constexpr uint64_t WifiMacRound(uint64_t half, uint64_t key, unsigned round) {
  return Mix64(half ^ (key + round * 0x9e3779b97f4a7c15ULL)) & kWifiMacHalfMask;
}

// rho: a keyed permutation of the 46 bits, and its inverse.
constexpr uint64_t WifiMacPermute(uint64_t bits, uint64_t key) {
  uint64_t left = bits >> kWifiMacHalfBits;
  uint64_t right = bits & kWifiMacHalfMask;
  for (unsigned round = 0; round < kWifiMacRounds; ++round) {
    const uint64_t next = left ^ WifiMacRound(right, key, round);
    left = right;
    right = next;
  }
  return left << kWifiMacHalfBits | right;
}

constexpr uint64_t WifiMacUnpermute(uint64_t bits, uint64_t key) {
  uint64_t left = bits >> kWifiMacHalfBits;
  uint64_t right = bits & kWifiMacHalfMask;
  for (unsigned round = kWifiMacRounds; round-- > 0;) {
    const uint64_t previous = right ^ WifiMacRound(left, key, round);
    right = left;
    left = previous;
  }
  return left << kWifiMacHalfBits | right;
}

// The address that |step| steps after eth0's along the cycle.
constexpr Mac WifiMacAt(const Mac& ethernet, uint64_t key, uint64_t step) {
  return WifiMacFromBits(
      WifiMacUnpermute((WifiMacPermute(WifiMacBits(ethernet), key) + step) % kWifiMacValues, key));
}

// |reserved| is the tests' to change: it tells which addresses are refused.
template <typename Reserved>
constexpr Mac DeriveWifiMacWith(const Mac& ethernet, uint64_t key, Reserved reserved) {
  Mac candidate = WifiMacAt(ethernet, key, 1);
  for (uint64_t step = 2; step <= kWifiMacMaxSteps && reserved(candidate); ++step) {
    candidate = WifiMacAt(ethernet, key, step);
  }
  return candidate;
}

constexpr Mac DeriveWifiMac(const Mac& ethernet, uint64_t key = kWifiMacKey) {
  return DeriveWifiMacWith(ethernet, key, WifiMacReserved);
}

// The eth0 address of a Wi-Fi address that DeriveWifiMac gave: the address
// before it on the cycle that is not reserved.
constexpr Mac EthernetFromWifiMac(const Mac& wifi, uint64_t key = kWifiMacKey) {
  const uint64_t position = WifiMacPermute(WifiMacBits(wifi), key);
  Mac candidate = WifiMacFromBits(WifiMacUnpermute((position + kWifiMacValues - 1) % kWifiMacValues, key));
  for (uint64_t step = 2; step <= kWifiMacMaxSteps && WifiMacReserved(candidate); ++step) {
    candidate = WifiMacFromBits(WifiMacUnpermute((position + kWifiMacValues - step) % kWifiMacValues, key));
  }
  return candidate;
}

}  // namespace kernarium_net

#endif  // KERNARIUM_WIFI_MAC_H_
