/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Example: download an S3 object directly into
 *          GPU VRAM via RDMA.
 *
 * Usage:
 *   get-object <size-bytes>              # stub callbacks (no S3)
 *   get-object <size-bytes> --live URL   # libcurl + test server
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <hip/hip_runtime.h>

#include "hipobj.h"

#if defined(HIPOBJ_HAVE_CURL)
#include <curl/curl.h>

#include "s3_curl_ops.h"
#endif

static int stubSendRequest(void* ctx, const char* token, size_t tokenLen) {
  (void)ctx;
  fprintf(stderr,
          "[get-object] would send RDMA token "
          "(%zu bytes) via S3 GET x-amz-rdma-token\n",
          tokenLen);
  return 0;
}

static int stubRecvReply(void* ctx, char* reply, size_t* replyLen) {
  (void)ctx;
  // No real server is contacted in stub mode; reply "501" (not
  // implemented) so the transfer is reported as unsupported rather
  // than silently succeeding.
  const char* reply501 = "501";
  size_t len = strlen(reply501);
  if (*replyLen < len) {
    return -1;
  }
  memcpy(reply, reply501, len);
  *replyLen = len;
  return 0;
}

#if defined(HIPOBJ_HAVE_CURL)
/* Check the GET's bytes against the checksums the server sent: Ceph's
 * x-amz-rdma-checksum-crc64nvme for the bytes delivered, and S3's
 * x-amz-checksum-crc64nvme for the whole object. */
static int verifyChecksums(void* devPtr, size_t size,
                           const hipObjS3CurlCtx& ctx) {
  const size_t got = ctx.bytesTransferred
                       ? static_cast<size_t>(ctx.bytesTransferred)
                       : size;
  char mine[HIPOBJ_CRC64NVME_B64_SIZE] = {};
  if (hipObjChecksumCrc64Nvme(devPtr, got, 0, mine).opError == hipObjSuccess) {
    fprintf(stdout, "CRC64NVME of the %zu bytes received: %s\n", got, mine);
  }
  struct {
    const char* header;
    const char* value;
  } sums[] = {{"x-amz-rdma-checksum-crc64nvme", ctx.rdmaChecksum},
              {"x-amz-checksum-crc64nvme", ctx.objectChecksum}};
  int rc = 0;
  for (const auto& sum : sums) {
    if (sum.value[0] == '\0') {
      continue;
    }
    hipObjError_t e = hipObjVerifyCrc64Nvme(devPtr, got, 0, sum.value);
    if (e.opError == hipObjInvalidValue) {
      /* a multipart composite is not a checksum of the bytes */
      fprintf(stdout, "%s %s: not verifiable, skipped\n", sum.header,
              sum.value);
      continue;
    }
    fprintf(stdout, "%s %s: %s\n", sum.header, sum.value,
            e.opError == hipObjSuccess ? "verified"
                                       : hipObjGetErrorString(e.opError));
    if (e.opError != hipObjSuccess) {
      rc = 1;
    }
  }
  return rc;
}
#endif

int main(int argc, char* argv[]) {
  if (argc < 2) {
    fprintf(stderr,
            "Usage: %s <object-size-bytes> [--live URL [bucket] [object]]\n",
            argv[0]);
    return 1;
  }

  size_t objSize = static_cast<size_t>(atol(argv[1]));
  if (objSize == 0) {
    fprintf(stderr, "Invalid object size: %s\n", argv[1]);
    return 1;
  }

  bool live = false;
  const char* endpoint = "http://127.0.0.1:9000";
  const char* bucket = "test";
  const char* object = "object";
  if (argc >= 4 && std::strcmp(argv[2], "--live") == 0) {
    live = true;
    endpoint = argv[3];
    if (argc >= 5) {
      bucket = argv[4];
    }
    if (argc >= 6) {
      object = argv[5];
    }
  }

  hipObjConfig_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.endpoint = endpoint;
  cfg.region = "us-east-1";
  cfg.gpuDevice = 0;
  cfg.nicHint = std::getenv("HIPOBJ_NIC_HINT");

  hipObjError_t err;
#if defined(HIPOBJECT_OFI_API)
  /* HIPOBJ_OFI_PROVIDER selects the libfabric transport: tcp, shm,
   * "verbs;ofi_rxm" or uet, as the server's writers run it. */
  hipObjOfiConfig_t ofi{};
  ofi.provider = std::getenv("HIPOBJ_OFI_PROVIDER");
  ofi.domain = std::getenv("HIPOBJ_OFI_DOMAIN");
  ofi.node = std::getenv("HIPOBJ_OFI_NODE");
  ofi.service = std::getenv("HIPOBJ_OFI_SERVICE");
  if (ofi.provider && ofi.provider[0] != '\0') {
    err = hipObjInitOfi(&cfg, &ofi);
  } else
#endif
  {
    err = hipObjInit(&cfg);
  }
  if (err.opError != hipObjSuccess) {
    fprintf(stderr, "hipObjInit failed: %s\n",
            hipObjGetErrorString(err.opError));
    return 1;
  }

  void* devPtr = nullptr;
  hipError_t hip_err = hipMalloc(&devPtr, objSize);
  if (hip_err != hipSuccess) {
    fprintf(stderr, "hipMalloc failed: %d\n", hip_err);
    hipObjShutdown();
    return 1;
  }

  err = hipObjBufRegister(devPtr, objSize);
  if (err.opError != hipObjSuccess) {
    fprintf(stderr, "hipObjBufRegister failed: %s\n",
            hipObjGetErrorString(err.opError));
    (void)hipFree(devPtr);
    hipObjShutdown();
    return 1;
  }

  hipObjOps_t ops;
  void* opsCtx = nullptr;
#if defined(HIPOBJ_HAVE_CURL)
  hipObjS3CurlCtx curlCtx{};
  if (live) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    curlCtx.endpoint = endpoint;
    curlCtx.bucket = bucket;
    curlCtx.object = object;
    curlCtx.objectSize = objSize;
    curlCtx.devPtr = devPtr;
    curlCtx.isPut = 0;
    curlCtx.accessKey = std::getenv("AWS_ACCESS_KEY_ID");
    curlCtx.secretKey = std::getenv("AWS_SECRET_ACCESS_KEY");
    curlCtx.region = std::getenv("AWS_REGION");
    ops.sendRequest = hipObjS3CurlSendRequest;
    ops.recvReply = hipObjS3CurlRecvReply;
    opsCtx = &curlCtx;
  } else
#endif
  {
    (void)live;
    ops.sendRequest = stubSendRequest;
    ops.recvReply = stubRecvReply;
  }

  int exitCode = 0;
  err = hipObjGet(nullptr, devPtr, objSize, 0, &ops, opsCtx);
  if (err.opError != hipObjSuccess) {
    fprintf(stderr, "hipObjGet failed: %s\n",
            hipObjGetErrorString(err.opError));
    exitCode = 1;
  } else {
    fprintf(stdout, "hipObjGet succeeded for %zu bytes\n", objSize);
#if defined(HIPOBJ_HAVE_CURL)
    if (live) {
      exitCode = verifyChecksums(devPtr, objSize, curlCtx);
    }
#endif
  }

#if defined(HIPOBJ_HAVE_CURL)
  if (live) {
    curl_global_cleanup();
  }
#endif

  hipObjBufDeregister(devPtr);
  (void)hipFree(devPtr);
  hipObjShutdown();
  return exitCode;
}
