/* Copyright (c) 2026 IBM Corporation. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* The libfabric transport, end to end on one host. An ofi-rma endpoint plays
 * the storage server: hipObjGet() hands it the ofi1 token through the
 * sendRequest callback, and it writes the object into the client's buffer
 * over tcp or shm before the reply comes back, as a Ceph OSD does. */

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <hip/hip_runtime.h>

#include <gtest/gtest.h>
#include <ofi_rma/ofi_rma.h>
#include <sys/uio.h>

#include "hipobj.h"

namespace {

constexpr std::chrono::milliseconds kWriteBudget{3000};

struct ProviderParam {
  const char* provider;
  const char* node;
};

/* The storage server's side of one GET. */
struct FakeServer {
  std::unique_ptr<ofi_rma::Endpoint> ep;
  std::vector<char> object;
  size_t objectOffset = 0;   /* the GET's offset into the object */
  bool write = true;         /* write the range before replying */
  std::string reply = "200"; /* x-amz-rdma-reply */
  std::string lastToken;
  int writeResult = 0;
};

int fakeSendRequest(void* ctx, const char* token, size_t tokenLen) {
  auto* s = static_cast<FakeServer*>(ctx);
  s->lastToken.assign(token, tokenLen);
  if (!s->write) {
    return 0;
  }
  const auto t = ofi_rma::parse_token(s->lastToken);
  if (!t || s->objectOffset + t->size > s->object.size()) {
    return -1;
  }
  iovec iov{s->object.data() + s->objectOffset, t->size};
  const std::vector<ofi_rma::Endpoint::write_t> writes = {{0, t->size, 0}};
  /* On a connected provider (verbs;ofi_rxm) a write with a revoked key
   * breaks the connection, and writes fail, flushed, until the provider
   * reconnects; a Ceph OSD delivers inline meanwhile, this server waits. */
  for (int i = 0; i < 50; ++i) {
    s->writeResult = s->ep->write(*t, &iov, 1, writes, kWriteBudget);
    if (s->writeResult != -ECANCELED) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return s->writeResult == 0 ? 0 : -1;
}

int fakeRecvReply(void* ctx, char* reply, size_t* replyLen) {
  auto* s = static_cast<FakeServer*>(ctx);
  if (*replyLen < s->reply.size()) {
    return -1;
  }
  std::memcpy(reply, s->reply.data(), s->reply.size());
  *replyLen = s->reply.size();
  return 0;
}

std::vector<char> pattern(size_t n, int seed) {
  std::vector<char> v(n);
  for (size_t i = 0; i < n; i++) {
    v[i] = static_cast<char>(i * 7 + seed);
  }
  return v;
}

/* tcp and shm, which need no hardware, and verbs;ofi_rxm when
 * OFI_RMA_TEST_VERBS_NODE names the address of an RDMA device, a soft-RoCE
 * one say. Soft-RoCE needs FI_UNIVERSE_SIZE=16, and re-keying a window
 * needs the verbs registration cache off; libfabric reads both once. */
std::vector<ProviderParam> testProviders() {
  std::vector<ProviderParam> v = {{"tcp", "127.0.0.1"}, {"shm", ""}};
  if (const char* node = std::getenv("OFI_RMA_TEST_VERBS_NODE");
      node && *node) {
    setenv("FI_UNIVERSE_SIZE", "16", 0);
    setenv("FI_MR_CACHE_MONITOR", "disabled", 0);
    v.push_back({"verbs;ofi_rxm", node});
  }
  return v;
}

/* gtest names allow letters, digits and underscores */
std::string providerTestName(
  const ::testing::TestParamInfo<ProviderParam>& info) {
  std::string n = info.param.provider;
  for (auto& c : n) {
    if (!std::isalnum(static_cast<unsigned char>(c))) {
      c = '_';
    }
  }
  return n;
}

bool haveGpu() {
  int n = 0;
  return hipGetDeviceCount(&n) == hipSuccess && n > 0;
}

class OfiTransportTest : public ::testing::TestWithParam<ProviderParam> {
protected:
  void SetUp() override {
    const ProviderParam& p = GetParam();
    ofi_rma::config_t c;
    c.provider = p.provider;
    c.node = p.node;
    c.stage_size = 4 << 20;
    c.stage_count = 2;
    std::string err;
    server_.ep = ofi_rma::Endpoint::open(c, &err);
    if (!server_.ep) {
      GTEST_SKIP() << p.provider << " is not available: " << err;
    }
    hipObjConfig_t cfg{};
    cfg.endpoint = "http://127.0.0.1:7480";
    cfg.region = "us-east-1";
    cfg.gpuDevice = -1;
    hipObjOfiConfig_t ofi{};
    ofi.provider = p.provider;
    ofi.node = *p.node ? p.node : nullptr;
    ASSERT_EQ(hipObjSuccess, hipObjInitOfi(&cfg, &ofi).opError);
    ops_.sendRequest = fakeSendRequest;
    ops_.recvReply = fakeRecvReply;
  }

  void TearDown() override {
    hipObjShutdown();
  }

  FakeServer server_;
  hipObjOps_t ops_{};
};

} // namespace

TEST_P(OfiTransportTest, GetPlacesTheObjectInAHostBuffer) {
  const size_t n = 2 << 20;
  server_.object = pattern(n, 3);
  std::vector<char> buf(n, 0);
  ASSERT_EQ(hipObjSuccess,
            hipObjBufRegisterHost(buf.data(), buf.size()).opError);
  ASSERT_EQ(hipObjSuccess,
            hipObjGet(nullptr, buf.data(), n, 0, &ops_, &server_).opError);
  EXPECT_EQ(0, std::memcmp(buf.data(), server_.object.data(), n));

  const auto t = ofi_rma::parse_token(server_.lastToken);
  ASSERT_TRUE(t);
  EXPECT_EQ(n, t->size);
  EXPECT_EQ(server_.ep->provider(), t->provider);
  EXPECT_EQ(hipObjSuccess, hipObjBufDeregister(buf.data()).opError);
}

TEST_P(OfiTransportTest, RangedGetLandsAtItsOffset) {
  const size_t n = 1 << 20;
  const size_t ofs = 4096;
  const size_t len = 8192;
  server_.object = pattern(n, 5);
  server_.objectOffset = ofs;
  std::vector<char> buf(n, 0);
  ASSERT_EQ(hipObjSuccess,
            hipObjBufRegisterHost(buf.data(), buf.size()).opError);
  ASSERT_EQ(hipObjSuccess,
            hipObjGet(nullptr, buf.data(), len, ofs, &ops_, &server_).opError);
  EXPECT_EQ(len, ofi_rma::parse_token(server_.lastToken)->size);
  EXPECT_EQ(0, std::memcmp(buf.data() + ofs, server_.object.data() + ofs, len));
  EXPECT_EQ(std::vector<char>(ofs, 0),
            std::vector<char>(buf.begin(), buf.begin() + ofs));
  EXPECT_EQ(std::vector<char>(n - ofs - len, 0),
            std::vector<char>(buf.begin() + ofs + len, buf.end()));
  /* Ceph's x-amz-rdma-checksum covers the delivered range, which S3's own
   * checksum headers cannot */
  char b64[HIPOBJ_CRC64NVME_B64_SIZE] = {};
  ASSERT_EQ(
    hipObjSuccess,
    hipObjChecksumCrc64Nvme(server_.object.data() + ofs, len, 0, b64).opError);
  const std::string rdmaChecksum = std::string("CRC64NVME ") + b64;
  EXPECT_EQ(
    hipObjSuccess,
    hipObjVerifyCrc64Nvme(buf.data(), len, ofs, rdmaChecksum.c_str()).opError);
  buf[ofs + 1] ^= 1;
  EXPECT_EQ(
    hipObjChecksumMismatch,
    hipObjVerifyCrc64Nvme(buf.data(), len, ofs, rdmaChecksum.c_str()).opError);
  /* a range past the buffer gets no token */
  EXPECT_NE(
    hipObjSuccess,
    hipObjGet(nullptr, buf.data(), len, n - 4096, &ops_, &server_).opError);
}

TEST_P(OfiTransportTest, DeclinedGetLeavesTheBufferAlone) {
  /* 501: the server sent the object in the HTTP body instead */
  const size_t n = 64 << 10;
  server_.object = pattern(n, 7);
  server_.write = false;
  server_.reply = "501";
  std::vector<char> buf(n, 0);
  ASSERT_EQ(hipObjSuccess,
            hipObjBufRegisterHost(buf.data(), buf.size()).opError);
  EXPECT_EQ(hipObjS3Error,
            hipObjGet(nullptr, buf.data(), n, 0, &ops_, &server_).opError);
  EXPECT_EQ(std::vector<char>(n, 0), buf);
}

TEST_P(OfiTransportTest, AFailedGetRetiresItsToken) {
  /* the server took the token but the GET failed; a write it makes later
   * must not land in the buffer, which the caller may already reuse */
  const size_t n = 64 << 10;
  server_.object = pattern(n, 9);
  server_.write = false;
  server_.reply = "500";
  std::vector<char> buf(n, 0);
  ASSERT_EQ(hipObjSuccess,
            hipObjBufRegisterHost(buf.data(), buf.size()).opError);
  EXPECT_NE(hipObjSuccess,
            hipObjGet(nullptr, buf.data(), n, 0, &ops_, &server_).opError);
  const auto stale = ofi_rma::parse_token(server_.lastToken);
  ASSERT_TRUE(stale);

  iovec iov{server_.object.data(), n};
  const std::vector<ofi_rma::Endpoint::write_t> all = {{0, n, 0}};
  EXPECT_NE(0, server_.ep->write(*stale, &iov, 1, all, kWriteBudget));
  EXPECT_EQ(std::vector<char>(n, 0), buf);

  /* the next GET gets a new key and works */
  server_.write = true;
  server_.reply = "200";
  ASSERT_EQ(hipObjSuccess,
            hipObjGet(nullptr, buf.data(), n, 0, &ops_, &server_).opError);
  EXPECT_NE(stale->key, ofi_rma::parse_token(server_.lastToken)->key);
  EXPECT_EQ(0, std::memcmp(buf.data(), server_.object.data(), n));
}

TEST_P(OfiTransportTest, PutIsNotSupported) {
  const size_t n = 64 << 10;
  std::vector<char> buf(n, 0);
  ASSERT_EQ(hipObjSuccess,
            hipObjBufRegisterHost(buf.data(), buf.size()).opError);
  EXPECT_EQ(hipObjOpNotSupported,
            hipObjPut(nullptr, buf.data(), n, 0, &ops_, &server_).opError);
  char* token = nullptr;
  EXPECT_EQ(
    hipObjOpNotSupported,
    hipObjGetRdmaToken(buf.data(), n, HIPOBJ_RDMA_OP_PUT, &token).opError);
  EXPECT_EQ(nullptr, token);
}

TEST_P(OfiTransportTest, TokenApiGetThenBufSync) {
  /* an application that sends the request itself */
  const size_t n = 256 << 10;
  server_.object = pattern(n, 11);
  std::vector<char> buf(n, 0);
  ASSERT_EQ(hipObjSuccess,
            hipObjBufRegisterHost(buf.data(), buf.size()).opError);
  char* token = nullptr;
  ASSERT_EQ(
    hipObjSuccess,
    hipObjGetRdmaToken(buf.data(), n, HIPOBJ_RDMA_OP_GET, &token).opError);
  ASSERT_NE(nullptr, token);
  ASSERT_EQ(0, fakeSendRequest(&server_, token, std::strlen(token)));
  EXPECT_EQ(hipObjSuccess,
            hipObjBufSync(buf.data(), n, 0, HIPOBJ_SYNC_TO_DEVICE).opError);
  EXPECT_EQ(0, std::memcmp(buf.data(), server_.object.data(), n));

  char nic[64] = {};
  EXPECT_EQ(hipObjSuccess,
            hipObjTokenClientNic(token, nic, sizeof nic).opError);
  if (std::string(GetParam().provider) == "tcp") {
    EXPECT_STREQ("127.0.0.1", nic);
  }
  EXPECT_EQ(hipObjSuccess, hipObjPutRdmaToken(token).opError);
}

TEST_P(OfiTransportTest, GetIntoAGpuBuffer) {
  /* GPU-direct when the provider offers FI_HMEM and libfabric has ROCr;
   * through a host staging buffer otherwise. Either way the bytes end up
   * in device memory. */
  if (!haveGpu()) {
    GTEST_SKIP() << "no HIP device";
  }
  const size_t n = 1 << 20;
  server_.object = pattern(n, 13);
  void* dev = nullptr;
  ASSERT_EQ(hipSuccess, hipMalloc(&dev, n));
  ASSERT_EQ(hipSuccess, hipMemset(dev, 0, n));
  ASSERT_EQ(hipSuccess, hipDeviceSynchronize());
  ASSERT_EQ(hipObjSuccess, hipObjBufRegister(dev, n).opError);
  EXPECT_EQ(hipObjSuccess,
            hipObjGet(nullptr, dev, n, 0, &ops_, &server_).opError);
  std::vector<char> got(n);
  ASSERT_EQ(hipSuccess, hipMemcpy(got.data(), dev, n, hipMemcpyDeviceToHost));
  EXPECT_EQ(0, std::memcmp(got.data(), server_.object.data(), n));
  EXPECT_EQ(hipObjSuccess, hipObjBufDeregister(dev).opError);
  EXPECT_EQ(hipSuccess, hipFree(dev));
}

INSTANTIATE_TEST_SUITE_P(Providers, OfiTransportTest,
                         ::testing::ValuesIn(testProviders()),
                         providerTestName);

TEST(OfiTransportInit, RefusesBadSettings) {
  hipObjConfig_t cfg{};
  cfg.gpuDevice = -1;
  hipObjOfiConfig_t ofi{};
  EXPECT_EQ(hipObjInvalidValue, hipObjInitOfi(&cfg, &ofi).opError);
  EXPECT_EQ(hipObjInvalidValue, hipObjInitOfi(nullptr, &ofi).opError);
  ofi.provider = "nosuchprov";
  EXPECT_EQ(hipObjRdmaError, hipObjInitOfi(&cfg, &ofi).opError);

  ofi.provider = "shm";
  if (hipObjInitOfi(&cfg, &ofi).opError != hipObjSuccess) {
    GTEST_SKIP() << "shm is not available";
  }
  EXPECT_EQ(hipObjAlreadyInitialized, hipObjInitOfi(&cfg, &ofi).opError);
  EXPECT_EQ(hipObjAlreadyInitialized, hipObjInit(&cfg).opError);
  EXPECT_EQ(hipObjSuccess, hipObjShutdown().opError);
  /* a second session after shutdown */
  EXPECT_EQ(hipObjSuccess, hipObjInitOfi(&cfg, &ofi).opError);
  EXPECT_EQ(hipObjSuccess, hipObjShutdown().opError);
}
