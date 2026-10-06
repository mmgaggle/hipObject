/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stddef.h>

#include "hipobj.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  const char* endpoint;
  const char* bucket;
  const char* object;
  const char* accessKey;
  const char* secretKey;
  int isPut;
  size_t objectSize;
  const void* devPtr;
  char lastReply[512];
  /* SigV4 region; NULL means us-east-1. Requests are signed when both
   * accessKey and secretKey are set. */
  const char* region;
  /* PUT: x-amz-checksum-crc64nvme to send, from hipObjChecksumCrc64Nvme(),
   * or empty. The server rejects a PUT whose bytes do not match. */
  char putChecksum[HIPOBJ_CRC64NVME_B64_SIZE];
  /* GET: from the last response, or empty. rdmaChecksum is Ceph's
   * x-amz-rdma-checksum, for the bytes delivered; objectChecksum is
   * x-amz-checksum-crc64nvme, for the whole object. */
  char rdmaChecksum[64];
  char objectChecksum[64];
  /* GET: x-amz-rdma-bytes-transferred, or 0 */
  unsigned long long bytesTransferred;
} hipObjS3CurlCtx;

int hipObjS3CurlSendRequest(void* ctx, const char* token, size_t tokenLen);
int hipObjS3CurlRecvReply(void* ctx, char* reply, size_t* replyLen);

#ifdef __cplusplus
}
#endif
