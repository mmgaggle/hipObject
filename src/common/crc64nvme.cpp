/* Copyright (c) 2026 IBM Corporation. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "crc64nvme.h"

#include <cstring>

namespace hipObj {

namespace {

/* CRC-64/NVME: polynomial 0xad93d23594c93659, reflected in and out, initial
 * value and final XOR all ones. Table-driven, eight bytes at a time. */
constexpr uint64_t kPolyReflected = 0x9a6c9329ac4bc9b5ULL;

struct Crc64Tables {
  uint64_t t[8][256];
};

const Crc64Tables& tables() {
  static const Crc64Tables tbl = [] {
    Crc64Tables x{};
    for (uint64_t i = 0; i < 256; ++i) {
      uint64_t c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1) ? (c >> 1) ^ kPolyReflected : c >> 1;
      }
      x.t[0][i] = c;
    }
    for (int i = 0; i < 256; ++i) {
      for (int s = 1; s < 8; ++s) {
        x.t[s][i] = (x.t[s - 1][i] >> 8) ^ x.t[0][x.t[s - 1][i] & 0xff];
      }
    }
    return x;
  }();
  return tbl;
}

constexpr char kB64[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int b64Value(char c) {
  const char* p = std::strchr(kB64, c);
  return (c != '\0' && p) ? static_cast<int>(p - kB64) : -1;
}

bool isSpace(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

} // namespace

uint64_t crc64nvme(uint64_t crc, const void* data, size_t len) {
  const auto& t = tables().t;
  const auto* p = static_cast<const unsigned char*>(data);
  uint64_t c = ~crc;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  while (len >= 8) {
    uint64_t v;
    std::memcpy(&v, p, sizeof(v));
    c ^= v;
    c = t[7][c & 0xff] ^ t[6][(c >> 8) & 0xff] ^ t[5][(c >> 16) & 0xff] ^
        t[4][(c >> 24) & 0xff] ^ t[3][(c >> 32) & 0xff] ^
        t[2][(c >> 40) & 0xff] ^ t[1][(c >> 48) & 0xff] ^ t[0][c >> 56];
    p += 8;
    len -= 8;
  }
#endif
  while (len--) {
    c = t[0][(c ^ *p++) & 0xff] ^ (c >> 8);
  }
  return ~c;
}

std::string armorCrc64nvme(uint64_t crc) {
  unsigned char b[8];
  for (int i = 0; i < 8; ++i) {
    b[i] = static_cast<unsigned char>(crc >> (56 - 8 * i));
  }
  std::string out;
  out.reserve(12);
  for (int i = 0; i < 8; i += 3) {
    const uint32_t n = (uint32_t{b[i]} << 16) |
                       (i + 1 < 8 ? uint32_t{b[i + 1]} << 8 : 0) |
                       (i + 2 < 8 ? uint32_t{b[i + 2]} : 0);
    out += kB64[(n >> 18) & 63];
    out += kB64[(n >> 12) & 63];
    out += i + 1 < 8 ? kB64[(n >> 6) & 63] : '=';
    out += i + 2 < 8 ? kB64[n & 63] : '=';
  }
  return out;
}

bool parseCrc64nvmeHeader(const std::string& value, uint64_t& crc) {
  size_t begin = 0;
  size_t end = value.size();
  while (begin < end && isSpace(value[begin])) {
    ++begin;
  }
  while (end > begin && isSpace(value[end - 1])) {
    --end;
  }
  static const char kPrefix[] = "CRC64NVME ";
  const size_t plen = sizeof(kPrefix) - 1;
  if (end - begin > plen && value.compare(begin, plen, kPrefix) == 0) {
    begin += plen;
    while (begin < end && isSpace(value[begin])) {
      ++begin;
    }
  }
  /* 8 bytes are 64 bits: 11 sextets carry them, the last with two zero
   * bits, then one '=' */
  if (end - begin != 12 || value[begin + 11] != '=') {
    return false;
  }
  uint64_t bits = 0;
  for (size_t i = 0; i < 11; ++i) {
    const int v = b64Value(value[begin + i]);
    if (v < 0) {
      return false;
    }
    if (i == 10) {
      if (v & 3) {
        return false; /* not canonical */
      }
      bits = (bits << 4) | static_cast<uint64_t>(v >> 2);
    } else {
      bits = (bits << 6) | static_cast<uint64_t>(v);
    }
  }
  crc = bits;
  return true;
}

} // namespace hipObj
