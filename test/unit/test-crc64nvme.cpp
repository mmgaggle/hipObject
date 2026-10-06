/* Copyright (c) 2026 IBM Corporation. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* CRC-64/NVME, its S3 base64 form, and the checksum API. */

#include <cstring>
#include <random>
#include <string>
#include <vector>

#include <hip/hip_runtime.h>

#include <gtest/gtest.h>

#include "crc64nvme.h"
#include "hipobj.h"

namespace {

/* One bit at a time, straight from the definition: an independent check on
 * the table-driven version. */
uint64_t referenceCrc64nvme(const void* data, size_t len) {
  const auto* p = static_cast<const unsigned char*>(data);
  uint64_t c = ~uint64_t{0};
  for (size_t i = 0; i < len; ++i) {
    c ^= p[i];
    for (int k = 0; k < 8; ++k) {
      c = (c & 1) ? (c >> 1) ^ 0x9a6c9329ac4bc9b5ULL : c >> 1;
    }
  }
  return ~c;
}

std::vector<char> randomBytes(size_t n, unsigned seed) {
  std::mt19937 rng(seed);
  std::vector<char> v(n);
  for (auto& c : v) {
    c = static_cast<char>(rng());
  }
  return v;
}

bool haveGpu() {
  int n = 0;
  return hipGetDeviceCount(&n) == hipSuccess && n > 0;
}

} // namespace

TEST(Crc64Nvme, MatchesTheCheckValue) {
  const char check[] = "123456789";
  EXPECT_EQ(0xae8b14860a799888ULL, hipObj::crc64nvme(0, check, 9));
  EXPECT_EQ("rosUhgp5mIg=", hipObj::armorCrc64nvme(0xae8b14860a799888ULL));
  EXPECT_EQ(0u, hipObj::crc64nvme(0, check, 0));
  EXPECT_EQ("AAAAAAAAAAA=", hipObj::armorCrc64nvme(0));
}

TEST(Crc64Nvme, MatchesTheReferenceAtEveryLengthAndAlignment) {
  const auto data = randomBytes(4096 + 64, 1);
  for (size_t ofs = 0; ofs < 8; ++ofs) {
    for (size_t len : {0, 1, 7, 8, 9, 15, 16, 63, 64, 65, 1000, 4096}) {
      EXPECT_EQ(referenceCrc64nvme(data.data() + ofs, len),
                hipObj::crc64nvme(0, data.data() + ofs, len))
        << "ofs " << ofs << " len " << len;
    }
  }
}

TEST(Crc64Nvme, ContinuesAcrossCalls) {
  const auto data = randomBytes(100000, 2);
  const uint64_t whole = hipObj::crc64nvme(0, data.data(), data.size());
  for (size_t split : {0, 1, 13, 4096, 99999, 100000}) {
    const uint64_t a = hipObj::crc64nvme(0, data.data(), split);
    EXPECT_EQ(whole,
              hipObj::crc64nvme(a, data.data() + split, data.size() - split))
      << "split " << split;
  }
}

TEST(Crc64Nvme, ParsesBothHeaderForms) {
  uint64_t crc = 0;
  EXPECT_TRUE(hipObj::parseCrc64nvmeHeader("rosUhgp5mIg=", crc));
  EXPECT_EQ(0xae8b14860a799888ULL, crc);
  crc = 0;
  EXPECT_TRUE(hipObj::parseCrc64nvmeHeader("CRC64NVME rosUhgp5mIg=", crc));
  EXPECT_EQ(0xae8b14860a799888ULL, crc);
  EXPECT_TRUE(
    hipObj::parseCrc64nvmeHeader("  CRC64NVME rosUhgp5mIg=\r\n", crc));

  for (const char* bad : {"", "CRC64NVME ", "rosUhgp5mIg", "rosUhgp5mIg==",
                          "rosUhgp5mIh=", /* not canonical: low bits set */
                          "rosUhgp5m!g=", "rosUhgp5mIg=-3", /* composite */
                          "CRC32C rosUhgp5mIg=", "crc64nvme rosUhgp5mIg="}) {
    EXPECT_FALSE(hipObj::parseCrc64nvmeHeader(bad, crc)) << bad;
  }
}

TEST(Crc64NvmeApi, ChecksumsAndVerifiesAHostBuffer) {
  const auto data = randomBytes(1 << 20, 3);
  char b64[HIPOBJ_CRC64NVME_B64_SIZE] = {};
  ASSERT_EQ(hipObjSuccess,
            hipObjChecksumCrc64Nvme(data.data(), data.size(), 0, b64).opError);
  EXPECT_EQ(hipObj::armorCrc64nvme(
              referenceCrc64nvme(data.data(), data.size())),
            b64);

  /* x-amz-checksum-crc64nvme, for the whole object */
  EXPECT_EQ(hipObjSuccess,
            hipObjVerifyCrc64Nvme(data.data(), data.size(), 0, b64).opError);

  /* x-amz-rdma-checksum, for a range */
  const size_t ofs = 4096;
  const size_t len = 65536;
  const std::string range = "CRC64NVME " +
                            hipObj::armorCrc64nvme(
                              referenceCrc64nvme(data.data() + ofs, len));
  EXPECT_EQ(
    hipObjSuccess,
    hipObjVerifyCrc64Nvme(data.data(), len, ofs, range.c_str()).opError);
  EXPECT_EQ(
    hipObjChecksumMismatch,
    hipObjVerifyCrc64Nvme(data.data(), len, ofs + 1, range.c_str()).opError);

  auto corrupt = data;
  corrupt[ofs + 100] ^= 1;
  EXPECT_EQ(
    hipObjChecksumMismatch,
    hipObjVerifyCrc64Nvme(corrupt.data(), len, ofs, range.c_str()).opError);

  EXPECT_EQ(hipObjInvalidValue,
            hipObjVerifyCrc64Nvme(data.data(), len, ofs, "garbage").opError);
  EXPECT_EQ(hipObjInvalidValue,
            hipObjVerifyCrc64Nvme(data.data(), len, ofs, nullptr).opError);
  EXPECT_EQ(hipObjInvalidValue,
            hipObjVerifyCrc64Nvme(data.data(), len, -1, range.c_str()).opError);
}

TEST(Crc64NvmeApi, ReadsDeviceMemoryThroughTheHost) {
  if (!haveGpu()) {
    GTEST_SKIP() << "no HIP device";
  }
  /* larger than one 4 MiB copy chunk, and not a multiple of it */
  const auto data = randomBytes((9 << 20) + 123, 4);
  void* dev = nullptr;
  ASSERT_EQ(hipSuccess, hipMalloc(&dev, data.size()));
  ASSERT_EQ(hipSuccess,
            hipMemcpy(dev, data.data(), data.size(), hipMemcpyHostToDevice));
  char b64[HIPOBJ_CRC64NVME_B64_SIZE] = {};
  ASSERT_EQ(hipObjSuccess,
            hipObjChecksumCrc64Nvme(dev, data.size(), 0, b64).opError);
  EXPECT_EQ(hipObj::armorCrc64nvme(
              hipObj::crc64nvme(0, data.data(), data.size())),
            b64);
  const std::string tail = "CRC64NVME " +
                           hipObj::armorCrc64nvme(
                             hipObj::crc64nvme(0, data.data() + 777, 5000));
  EXPECT_EQ(hipObjSuccess,
            hipObjVerifyCrc64Nvme(dev, 5000, 777, tail.c_str()).opError);
  EXPECT_EQ(hipSuccess, hipFree(dev));
}
