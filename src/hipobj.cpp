/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "hipobj.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <hip/hip_runtime.h>

#include "buffer.h"
#include "control.h"
#include "crc64nvme.h"
#include "hip-seam.h"
#include "hipobj-private.h"
#include "ibv-wrapper.h"
#include "rdma-topology.h"
#include "state.h"
#include "token.h"
#include "transport.h"
#ifdef HIPOBJECT_V2_API
#include "v2-registry.h"
#include "v2-transport.h"
#endif
#ifdef HIPOBJECT_OFI_API
#include "ofi-transport.h"
#endif

namespace hipObj {

static BufferMap g_bufferMap;
static RcConnection g_conn;
#ifdef HIPOBJECT_OFI_API
static OfiTransport g_ofi;
#endif

static hipObjError_t handleException() {
  try {
    throw;
  } catch (const std::exception&) {
    return {hipObjInternalError, 0};
  } catch (...) {
    return {hipObjInternalError, 0};
  }
}

static bool buildRdmaToken(const void* devPtr, size_t size, off_t offset,
                           RdmaToken& token) {
  struct ibv_mr* mr = g_bufferMap.lookupMr(const_cast<void*>(devPtr));
  if (!mr || !g_conn.qp) {
    return false;
  }
  size_t regSize = g_bufferMap.lookupSize(const_cast<void*>(devPtr));
  if (offset < 0 || static_cast<size_t>(offset) + size > regSize) {
    return false;
  }
  token.transport = TRANSPORT_RC;
  token.qpNum = g_conn.qp->qp_num;
  std::memcpy(token.gid, g_conn.localGid.raw, 16);
  token.rkey = mr->rkey;
  token.remoteAddr = g_bufferMap.lookupRemoteAddr(const_cast<void*>(devPtr)) +
                     static_cast<uint64_t>(offset);
  token.length = static_cast<uint64_t>(size);
  token.portNum = g_conn.portNum;
  token.lid = 0;
  return true;
}

static int finishTransferAfterReply(const char* reply, size_t replyLen,
                                    bool requiresDeviceSync) {
  RdmaToken peerToken{};
  int httpCode = 0;
  if (parsePeerTokenFromReply(reply, replyLen, peerToken, httpCode)) {
    if (connectRcPeer(g_conn, peerToken) != 0) {
      return -1;
    }
  }
  // A poll timeout or error completion is a failure: the transfer outcome
  // is unknown. With the current one-sided protocol the responder side
  // may not observe a completion at all (see issue #22), so this check
  // reports "no evidence of failure" rather than "transfer verified".
  if (pollCompletion(g_conn, -1, 5000) != 0) {
    return -1;
  }
  if (!requiresDeviceSync) {
    return 0;
  }
  hipError_t err = hipObj::hipOps().hipDeviceSynchronize();
  return (err == hipSuccess) ? 0 : -1;
}

/* Milliseconds to wait for a staging copy before giving up. A 1 MiB copy is
 * single-digit milliseconds on real hardware and ~170 ms on the emulated GPU
 * the CI lanes use, so the default is several orders of magnitude of slack.
 * It exists for one reason: a DMA that never completes must not become an
 * unkillable process. ROCm waits on the completion signal with
 * BusyWaitSignal::WaitRelaxed, which spins in userspace rather than blocking,
 * so a wedged copy shows up as a busy core and no kernel log at all -- there
 * is no other layer that will ever time this out. */
static long stageTimeoutMs() {
  static long ms = [] {
    const char* env = getenv("HIPOBJ_STAGE_TIMEOUT_MS");
    if (!env || !*env) {
      return 30000L;
    }
    char* end = nullptr;
    long v = strtol(env, &end, 10);
    return (end && *end == '\0' && v > 0) ? v : 30000L;
  }();
  return ms;
}

/* hipMemcpy with a deadline. Async copy plus a recorded event polled to a
 * wall-clock bound: on timeout the event and the copy are abandoned
 * deliberately -- the copy owns the staging buffer and the stream, and there
 * is no safe way to reclaim either while the DMA may still land. The caller
 * gets an error instead of a hang, which is the whole point. */
static hipObjError_t stageCopyWithDeadline(void* dev, void* host, size_t size,
                                           bool toDevice) {
  HipOps& ops = hipOps();
  const hipMemcpyKind kind = toDevice ? hipMemcpyHostToDevice
                                      : hipMemcpyDeviceToHost;
  void* dst = toDevice ? dev : host;
  const void* src = toDevice ? host : dev;

  /* Unbounded fallback, for a table that cannot express the bounded form --
   * a test seam with only the classic entries, or a HIP runtime too old to
   * have events. */
  auto blockingCopy = [&]() -> hipObjError_t {
    if (!ops.hipMemcpy) {
      return {hipObjInternalError, 0};
    }
    hipError_t err = ops.hipMemcpy(dst, src, size, kind);
    return (err == hipSuccess) ? HIPOBJ_SUCCESS
                               : hipObjError_t{hipObjInternalError, 0};
  };

  if (!ops.hipMemcpyAsync || !ops.hipEventCreate || !ops.hipEventRecord ||
      !ops.hipEventQuery || !ops.hipEventDestroy) {
    return blockingCopy();
  }

  hipEvent_t done = nullptr;
  if (ops.hipEventCreate(&done) != hipSuccess) {
    return blockingCopy();
  }

  hipError_t err = ops.hipMemcpyAsync(dst, src, size, kind, nullptr);
  if (err == hipSuccess) {
    err = ops.hipEventRecord(done, nullptr);
  }
  if (err != hipSuccess) {
    (void)ops.hipEventDestroy(done);
    return {hipObjInternalError, 0};
  }

  const long timeoutMs = stageTimeoutMs();
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeoutMs);
  for (;;) {
    hipError_t q = ops.hipEventQuery(done);
    if (q == hipSuccess) {
      (void)ops.hipEventDestroy(done);
      return HIPOBJ_SUCCESS;
    }
    if (q != hipErrorNotReady) {
      (void)ops.hipEventDestroy(done);
      return {hipObjInternalError, 0};
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      fprintf(stderr,
              "hipObj: staging %s copy of %zu bytes did not complete within "
              "%ld ms; abandoning it. The GPU never signalled completion -- "
              "see HIPOBJ_STAGE_TIMEOUT_MS.\n",
              toDevice ? "host-to-device" : "device-to-host", size, timeoutMs);
      return {hipObjInternalError, 0};
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}
static hipObjError_t stageBuffer(void* devPtr, size_t size, off_t offset,
                                 bool toDevice) {
  void* hostBuf = nullptr;
  size_t regSize = 0;
#ifdef HIPOBJECT_OFI_API
  if (getState().ofi) {
    hostBuf = g_ofi.lookupHostBuf(devPtr);
    regSize = g_ofi.lookupSize(devPtr);
  } else
#endif
  {
    hostBuf = g_bufferMap.lookupHostBuf(devPtr);
    regSize = g_bufferMap.lookupSize(devPtr);
  }
  if (!hostBuf) {
    return HIPOBJ_SUCCESS; /* the NIC reads and writes the caller's memory */
  }
  if (offset < 0 || static_cast<size_t>(offset) + size > regSize) {
    return {hipObjInvalidValue, 0};
  }
  void* host = static_cast<char*>(hostBuf) + offset;
  void* dev = static_cast<char*>(devPtr) + offset;
  return stageCopyWithDeadline(dev, host, size, toDevice);
}

static hipObjError_t runRdmaTransfer(const void* devPtr, size_t size,
                                     off_t offset, hipObjOps_t* ops,
                                     void* ctx) {
  bool requiresDeviceSync = g_bufferMap.requiresDeviceSync(
    const_cast<void*>(devPtr));
  RdmaToken token{};
  if (!buildRdmaToken(devPtr, size, offset, token)) {
    return {hipObjRdmaError, 0};
  }
  std::string encoded = encodeRdmaToken(token);
  if (injectRdmaToken(ops, ctx, encoded) != 0) {
    return {hipObjS3Error, 0};
  }
  char replyBuf[512];
  size_t replyLen = sizeof(replyBuf);
  int rdmaStatus = 0;
  if (receiveRdmaReplyRaw(ops, ctx, replyBuf, &replyLen, rdmaStatus) != 0 ||
      rdmaStatus != 0) {
    return {hipObjS3Error, 0};
  }
  if (finishTransferAfterReply(replyBuf, replyLen, requiresDeviceSync) != 0) {
    return {hipObjRdmaError, 0};
  }
  return HIPOBJ_SUCCESS;
}

/* Hand a minted token to the caller, who frees it with hipObjPutRdmaToken(). */
static hipObjError_t copyTokenOut(const std::string& encoded, char** outToken) {
  char* copy = static_cast<char*>(std::malloc(encoded.size() + 1));
  if (!copy) {
    return {hipObjInternalError, 0};
  }
  std::memcpy(copy, encoded.c_str(), encoded.size() + 1);
  *outToken = copy;
  return HIPOBJ_SUCCESS;
}

/* CRC-64/NVME of [ptr, ptr + size). Device memory is copied to the host a
 * chunk at a time; host memory is read in place. */
static int bufferCrc64nvme(const void* ptr, size_t size, uint64_t& crc) {
  const char* p = static_cast<const char*>(ptr);
  if (!isDevicePointer(const_cast<char*>(p))) {
    crc = crc64nvme(0, p, size);
    return 0;
  }
  auto memcpyFn = hipOps().hipMemcpy;
  if (!memcpyFn) {
    return -1;
  }
  constexpr size_t kChunk = 4 << 20;
  std::vector<char> host(size < kChunk ? size : kChunk);
  uint64_t c = 0;
  for (size_t done = 0; done < size;) {
    const size_t n = size - done < kChunk ? size - done : kChunk;
    if (memcpyFn(host.data(), p + done, n, hipMemcpyDeviceToHost) !=
        hipSuccess) {
      return -1;
    }
    c = crc64nvme(c, host.data(), n);
    done += n;
  }
  crc = c;
  return 0;
}

static void resetDriverState(DriverState& state) {
  state.initialized = false;
  state.gpuDevice = 0;
  state.endpoint.clear();
  state.region.clear();
  state.nicHint.clear();
  state.nicIndex = -1;
  state.flags = 0;
  state.ofi = false;
}

#ifdef HIPOBJECT_OFI_API
static hipObjError_t ofiRegister(void* ptr, size_t size, bool hostMemory) {
  if (!ptr) {
    return {hipObjInvalidValue, 0};
  }
  if (g_ofi.isRegistered(ptr)) {
    return {hipObjBufAlreadyRegistered, 0};
  }
  return g_ofi.registerBuffer(ptr, size, hostMemory) == 0
           ? HIPOBJ_SUCCESS
           : hipObjError_t{hipObjRdmaError, 0};
}

/* A GET over the libfabric transport. The server's writers place the object
 * in the window before the reply comes back; the endpoint thread places it,
 * so sync() orders this thread's reads (and the staging copy) after it. */
static hipObjError_t runOfiGet(void* devPtr, size_t size, off_t offset,
                               hipObjOps_t* ops, void* ctx) {
  if (!g_ofi.isRegistered(devPtr)) {
    return {hipObjBufNotRegistered, 0};
  }
  if (offset < 0) {
    return {hipObjInvalidValue, 0};
  }
  std::string token = g_ofi.makeToken(devPtr, size,
                                      static_cast<size_t>(offset));
  if (token.empty()) {
    return {hipObjRdmaError, 0};
  }
  bool completed = false;
  hipObjError_t err = HIPOBJ_SUCCESS;
  int rdmaStatus = 0;
  if (injectRdmaToken(ops, ctx, token) != 0) {
    err = {hipObjS3Error, 0};
  } else if (receiveRdmaReply(ops, ctx, rdmaStatus) != 0 || rdmaStatus != 0) {
    /* rdmaStatus -2 is a 501: the server sent the object in the HTTP body,
     * which the caller's own callbacks received */
    err = {hipObjS3Error, 0};
  } else {
    completed = true;
    g_ofi.sync();
    if (g_ofi.isDirectDevice(devPtr) &&
        hipOps().hipDeviceSynchronize() != hipSuccess) {
      err = {hipObjRdmaError, 0};
    } else {
      err = stageBuffer(devPtr, size, offset, true);
    }
  }
  g_ofi.retire(devPtr, completed);
  return err;
}

/* A PUT whose payload stays in the buffer: the server's OSDs read it out of
 * the window that the token names (an OSD-direct PUT). The request carries
 * no body. A server that cannot take it that way answers 501, and the
 * caller then sends the body over HTTP. */
static hipObjError_t runOfiPut(const void* constPtr, size_t size, off_t offset,
                               hipObjOps_t* ops, void* ctx) {
  void* devPtr = const_cast<void*>(constPtr);
  if (!g_ofi.isRegistered(devPtr)) {
    return {hipObjBufNotRegistered, 0};
  }
  if (offset < 0) {
    return {hipObjInvalidValue, 0};
  }
  if (!g_ofi.reads()) {
    /* the provider cannot let the server read the window */
    return {hipObjOpNotSupported, 0};
  }
  /* the window must hold the bytes before the server reads them: copy a
   * buffer staged through host memory, and wait for the GPU's writes to a
   * buffer lent directly */
  hipObjError_t err = stageBuffer(devPtr, size, offset, false);
  if (err.opError != hipObjSuccess) {
    return err;
  }
  if (g_ofi.isDirectDevice(devPtr) &&
      hipOps().hipDeviceSynchronize() != hipSuccess) {
    return {hipObjRdmaError, 0};
  }
  std::string token = g_ofi.makeToken(devPtr, size,
                                      static_cast<size_t>(offset));
  if (token.empty()) {
    return {hipObjRdmaError, 0};
  }
  bool completed = false;
  int rdmaStatus = 0;
  if (injectRdmaToken(ops, ctx, token) != 0) {
    err = {hipObjS3Error, 0};
  } else if (receiveRdmaReply(ops, ctx, rdmaStatus) != 0 || rdmaStatus != 0) {
    /* rdmaStatus -2 is a 501: the server stored nothing, and the caller
     * sends the body instead */
    err = {hipObjS3Error, 0};
  } else {
    completed = true;
  }
  g_ofi.retire(devPtr, completed);
  return err;
}
#endif /* HIPOBJECT_OFI_API */

} // namespace hipObj

extern "C" {

const char* hipObjGetErrorString(hipObjOpError_t err) {
  try {
    switch (err) {
      case hipObjSuccess:
        return "Success";
      case hipObjInvalidValue:
        return "Invalid value";
      case hipObjNotInitialized:
        return "Not initialized";
      case hipObjAlreadyInitialized:
        return "Already initialized";
      case hipObjRdmaError:
        return "RDMA error";
      case hipObjS3Error:
        return "S3 error";
      case hipObjBufNotRegistered:
        return "Buffer not registered";
      case hipObjBufAlreadyRegistered:
        return "Buffer already registered";
      case hipObjNicNotFound:
        return "NIC not found";
      case hipObjDmabufNotSupported:
        return "dmabuf not supported";
      case hipObjSizeTooLarge:
        return "Size too large";
      case hipObjInternalError:
        return "Internal error";
#ifdef HIPOBJECT_OFI_API
      case hipObjOpNotSupported:
        return "Operation not supported by the libfabric transport";
#endif /* HIPOBJECT_OFI_API */
      case hipObjChecksumMismatch:
        return "Checksum mismatch";
#ifdef HIPOBJECT_V2_API
      case hipObjNotSupported:
        return "hipobj-rc-v2 not supported by server";
      case hipObjBusy:
        return "Server busy (backpressure)";
#endif /* HIPOBJECT_V2_API */
      default:
        return "Unknown error";
    }
  } catch (...) {
    return "Unknown error";
  }
}

hipObjError_t hipObjInit(hipObjConfig_t* config) try {
  if (!config) {
    return {hipObjInvalidValue, 0};
  }
  hipObj::DriverState& state = hipObj::getState();
  if (state.initialized) {
    return {hipObjAlreadyInitialized, 0};
  }
  if (!hipObj::ibv.is_initialized) {
    return {hipObjRdmaError, 0};
  }
  const bool haveNicHint = config->nicHint && config->nicHint[0] != '\0';
  int gpuDevice = config->gpuDevice;
  if (gpuDevice < 0) {
    hipError_t err = hipObj::hipOps().hipGetDevice(&gpuDevice);
    if (err != hipSuccess) {
      // No GPU to infer a device from. That is only fatal when we also have
      // no NIC hint -- with a hint the topology lookup below is skipped
      // entirely, so an absent GPU is not an error.
      if (err != hipErrorNoDevice || !haveNicHint) {
        return {hipObjRdmaError, static_cast<int>(err)};
      }
      gpuDevice = -1;
    }
  }
  const char* devName = nullptr;
  int nicIndex = -1;
  if (gpuDevice >= 0) {
    nicIndex = hipObj::GetClosestNicToGpu(gpuDevice,
                                          config->nicHint ? config->nicHint
                                                          : nullptr,
                                          &devName);
  }
  if (nicIndex < 0) {
    // GPU topology lookup failed (no GPU or no matching NIC). When a NIC name
    // hint is provided, try opening it directly without GPU topology so the
    // library works in GPU-less environments (e.g. CI with emulated RDMA).
    if (haveNicHint) {
      devName = config->nicHint;
      nicIndex = 0;
    } else {
      return {hipObjNicNotFound, 0};
    }
  }
  int ret = (devName) ? hipObj::openRdmaDeviceByName(devName, hipObj::g_conn)
                      : hipObj::openRdmaDevice(nicIndex, hipObj::g_conn);
  if (ret != 0) {
    return {hipObjRdmaError, 0};
  }
  ret = hipObj::createRcQp(hipObj::g_conn, 256, 128, 128);
  if (ret != 0) {
    hipObj::closeRdmaDevice(hipObj::g_conn);
    return {hipObjRdmaError, 0};
  }
  ret = hipObj::transitionQpToInit(hipObj::g_conn);
  if (ret != 0) {
    hipObj::closeRdmaDevice(hipObj::g_conn);
    return {hipObjRdmaError, 0};
  }
  state.initialized = true;
  state.gpuDevice = gpuDevice;
  state.endpoint = config->endpoint ? config->endpoint : "";
  state.region = config->region ? config->region : "";
  state.nicHint = config->nicHint ? config->nicHint : "";
  state.nicIndex = nicIndex;
  state.flags = config->flags;
  return HIPOBJ_SUCCESS;
} catch (...) {
  return hipObj::handleException();
}

#ifdef HIPOBJECT_OFI_API
hipObjError_t hipObjInitOfi(hipObjConfig_t* config,
                            const hipObjOfiConfig_t* ofi) try {
  if (!config || !ofi || !ofi->provider || ofi->provider[0] == '\0') {
    return {hipObjInvalidValue, 0};
  }
  hipObj::DriverState& state = hipObj::getState();
  if (state.initialized) {
    return {hipObjAlreadyInitialized, 0};
  }
  /* No verbs device and no NIC topology lookup: the provider and its domain
   * name the NIC. A host without a GPU lends host buffers only. */
  int gpuDevice = config->gpuDevice;
  if (gpuDevice < 0) {
    auto getDevice = hipObj::hipOps().hipGetDevice;
    if (!getDevice || getDevice(&gpuDevice) != hipSuccess) {
      gpuDevice = -1;
    }
  }
  std::string err;
  if (hipObj::g_ofi.open(*ofi, gpuDevice, &err) != 0) {
    fprintf(stderr, "hipObj: libfabric endpoint (%s): %s\n", ofi->provider,
            err.c_str());
    return {hipObjRdmaError, 0};
  }
  state.initialized = true;
  state.gpuDevice = gpuDevice;
  state.endpoint = config->endpoint ? config->endpoint : "";
  state.region = config->region ? config->region : "";
  state.nicHint = ofi->domain ? ofi->domain : "";
  state.nicIndex = -1;
  state.flags = config->flags;
  state.ofi = true;
  return HIPOBJ_SUCCESS;
} catch (...) {
  return hipObj::handleException();
}
#endif /* HIPOBJECT_OFI_API */

hipObjError_t hipObjShutdown(void) try {
  hipObj::DriverState& state = hipObj::getState();
  if (!state.initialized) {
    return HIPOBJ_SUCCESS;
  }
#ifdef HIPOBJECT_OFI_API
  if (state.ofi) {
    hipObj::g_ofi.close();
    hipObj::resetDriverState(state);
    return HIPOBJ_SUCCESS;
  }
#endif
#ifdef HIPOBJECT_V2_API
  std::lock_guard<std::mutex> apiGuard(hipObj::v2::apiLock());
  /* v2 first: release every connection (destroy retries included);
   * leftover poison must stop the teardown so the failure is
   * visible instead of violating the PD/context lifetime rule. */
  bool poisonLeft = false;
  hipObj::v2::ConnectionRegistry& reg = hipObj::v2::registry();
  std::vector<hipObj::v2::ConnId> ids;
  reg.forEachId([&ids](hipObj::v2::ConnId id) {
    ids.push_back(id);
  });
  for (auto id : ids) {
    int rc = hipObj::v2::releaseConnection(id);
    if (rc == hipObj::v2::kReleaseLeftover) {
      poisonLeft = true;
    }
  }
  if (poisonLeft || reg.size() > 0) {
    return {hipObjRdmaError, 0};
  }
#endif /* HIPOBJECT_V2_API */
  hipObj::g_bufferMap.deregisterAll();
  hipObj::closeRdmaDevice(hipObj::g_conn);
  hipObj::resetDriverState(state);
  return HIPOBJ_SUCCESS;
} catch (...) {
  return hipObj::handleException();
}

hipObjError_t hipObjBufRegister(void* devPtr, size_t size) try {
  hipObj::DriverState& state = hipObj::getState();
  if (!state.initialized) {
    return {hipObjNotInitialized, 0};
  }
  if (size > hipObj::MAX_MR_SIZE) {
    return {hipObjSizeTooLarge, 0};
  }
#ifdef HIPOBJECT_OFI_API
  if (state.ofi) {
    return hipObj::ofiRegister(devPtr, size, false);
  }
#endif
  if (hipObj::g_bufferMap.isRegistered(devPtr)) {
    return {hipObjBufAlreadyRegistered, 0};
  }
  int ret = hipObj::g_bufferMap.registerBuffer(devPtr, size, hipObj::g_conn.pd);
  if (ret != 0) {
    return {hipObjRdmaError, 0};
  }
  return HIPOBJ_SUCCESS;
} catch (...) {
  return hipObj::handleException();
}

hipObjError_t hipObjBufRegisterHost(void* hostPtr, size_t size) try {
  hipObj::DriverState& state = hipObj::getState();
  if (!state.initialized) {
    return {hipObjNotInitialized, 0};
  }
  if (!hostPtr) {
    return {hipObjInvalidValue, 0};
  }
  if (size > hipObj::MAX_MR_SIZE) {
    return {hipObjSizeTooLarge, 0};
  }
#ifdef HIPOBJECT_OFI_API
  if (state.ofi) {
    return hipObj::ofiRegister(hostPtr, size, true);
  }
#endif
  if (hipObj::g_bufferMap.isRegistered(hostPtr)) {
    return {hipObjBufAlreadyRegistered, 0};
  }
  int ret = hipObj::g_bufferMap.registerHostBuffer(hostPtr, size,
                                                   hipObj::g_conn.pd);
  if (ret != 0) {
    return {hipObjRdmaError, 0};
  }
  return HIPOBJ_SUCCESS;
} catch (...) {
  return hipObj::handleException();
}

hipObjError_t hipObjBufDeregister(void* devPtr) try {
  hipObj::DriverState& state = hipObj::getState();
  if (!state.initialized) {
    return {hipObjNotInitialized, 0};
  }
#ifdef HIPOBJECT_OFI_API
  if (state.ofi) {
    if (!hipObj::g_ofi.isRegistered(devPtr)) {
      return {hipObjBufNotRegistered, 0};
    }
    return hipObj::g_ofi.deregisterBuffer(devPtr) == 0
             ? HIPOBJ_SUCCESS
             : hipObjError_t{hipObjRdmaError, 0};
  }
#endif
  if (!hipObj::g_bufferMap.isRegistered(devPtr)) {
    return {hipObjBufNotRegistered, 0};
  }
  int ret = hipObj::g_bufferMap.deregisterBuffer(devPtr);
  if (ret != 0) {
    return {hipObjRdmaError, 0};
  }
  return HIPOBJ_SUCCESS;
} catch (...) {
  return hipObj::handleException();
}

hipObjError_t hipObjGet(hipObjHandle_t handle, void* devPtr, size_t size,
                        off_t offset, hipObjOps_t* ops, void* ctx) try {
  (void)handle;
  hipObj::DriverState& state = hipObj::getState();
  if (!state.initialized) {
    return {hipObjNotInitialized, 0};
  }
  if (!ops) {
    return {hipObjInvalidValue, 0};
  }
#ifdef HIPOBJECT_OFI_API
  if (state.ofi) {
    return hipObj::runOfiGet(devPtr, size, offset, ops, ctx);
  }
#endif
  if (!hipObj::g_bufferMap.lookupMr(devPtr)) {
    return {hipObjBufNotRegistered, 0};
  }
  hipObjError_t err = hipObj::runRdmaTransfer(devPtr, size, offset, ops, ctx);
  if (err.opError != hipObjSuccess) {
    return err;
  }
  return hipObj::stageBuffer(devPtr, size, offset, true);
} catch (...) {
  return hipObj::handleException();
}

hipObjError_t hipObjPut(hipObjHandle_t handle, const void* devPtr, size_t size,
                        off_t offset, hipObjOps_t* ops, void* ctx) try {
  (void)handle;
  hipObj::DriverState& state = hipObj::getState();
  if (!state.initialized) {
    return {hipObjNotInitialized, 0};
  }
  if (!ops) {
    return {hipObjInvalidValue, 0};
  }
#ifdef HIPOBJECT_OFI_API
  if (state.ofi) {
    return hipObj::runOfiPut(devPtr, size, offset, ops, ctx);
  }
#endif
  if (!hipObj::g_bufferMap.lookupMr(const_cast<void*>(devPtr))) {
    return {hipObjBufNotRegistered, 0};
  }
  hipObjError_t serr = hipObj::stageBuffer(const_cast<void*>(devPtr), size,
                                           offset, false);
  if (serr.opError != hipObjSuccess) {
    return serr;
  }
  return hipObj::runRdmaTransfer(devPtr, size, offset, ops, ctx);
} catch (...) {
  return hipObj::handleException();
}

hipObjError_t hipObjBufSync(void* devPtr, size_t size, off_t offset,
                            int direction) try {
  hipObj::DriverState& state = hipObj::getState();
  if (!state.initialized) {
    return {hipObjNotInitialized, 0};
  }
  if (!devPtr || (direction != HIPOBJ_SYNC_TO_HOST &&
                  direction != HIPOBJ_SYNC_TO_DEVICE)) {
    return {hipObjInvalidValue, 0};
  }
#ifdef HIPOBJECT_OFI_API
  if (state.ofi) {
    if (!hipObj::g_ofi.isRegistered(devPtr)) {
      return {hipObjBufNotRegistered, 0};
    }
    /* After a GET made with hipObjGetRdmaToken(): order the reads, and the
     * staging copy, after the writes the endpoint thread placed. */
    hipObj::g_ofi.sync();
    return hipObj::stageBuffer(devPtr, size, offset,
                               direction == HIPOBJ_SYNC_TO_DEVICE);
  }
#endif
  if (!hipObj::g_bufferMap.isRegistered(devPtr)) {
    return {hipObjBufNotRegistered, 0};
  }
  return hipObj::stageBuffer(devPtr, size, offset,
                             direction == HIPOBJ_SYNC_TO_DEVICE);
} catch (...) {
  return hipObj::handleException();
}

hipObjError_t hipObjGetRdmaToken(const void* devPtr, size_t size, int op,
                                 char** outToken) {
  return hipObjGetRdmaTokenAt(devPtr, size, 0, op, outToken);
}

hipObjError_t hipObjGetRdmaTokenAt(const void* devPtr, size_t size,
                                   size_t offset, int op, char** outToken) try {
  hipObj::DriverState& state = hipObj::getState();
  if (!state.initialized) {
    return {hipObjNotInitialized, 0};
  }
  if (!devPtr || !outToken || size == 0) {
    return {hipObjInvalidValue, 0};
  }
  if (op != HIPOBJ_RDMA_OP_PUT && op != HIPOBJ_RDMA_OP_GET) {
    return {hipObjInvalidValue, 0};
  }
#ifdef HIPOBJECT_OFI_API
  if (state.ofi) {
    /* a PUT token lets the server read the window */
    if (op == HIPOBJ_RDMA_OP_PUT && !hipObj::g_ofi.reads()) {
      return {hipObjOpNotSupported, 0};
    }
    void* ptr = const_cast<void*>(devPtr);
    if (!hipObj::g_ofi.isRegistered(ptr)) {
      return {hipObjBufNotRegistered, 0};
    }
    const size_t regSize = hipObj::g_ofi.lookupSize(ptr);
    if (offset > regSize || size > regSize - offset) {
      return {hipObjInvalidValue, 0};
    }
    std::string encoded = hipObj::g_ofi.makeToken(ptr, size, offset);
    if (encoded.empty()) {
      return {hipObjRdmaError, 0};
    }
    return hipObj::copyTokenOut(encoded, outToken);
  }
#endif
  if (!hipObj::g_bufferMap.lookupMr(const_cast<void*>(devPtr))) {
    return {hipObjBufNotRegistered, 0};
  }
  const size_t regSize = hipObj::g_bufferMap.lookupSize(
    const_cast<void*>(devPtr));
  if (offset > regSize || size > regSize - offset) {
    return {hipObjInvalidValue, 0};
  }
  hipObj::RdmaToken token{};
  if (!hipObj::buildRdmaToken(devPtr, size, static_cast<off_t>(offset),
                              token)) {
    return {hipObjRdmaError, 0};
  }
  return hipObj::copyTokenOut(hipObj::encodeRdmaToken(token), outToken);
} catch (...) {
  return hipObj::handleException();
}

hipObjError_t hipObjPutRdmaToken(char* token) try {
  if (!token) {
    return {hipObjInvalidValue, 0};
  }
  std::free(token);
  return HIPOBJ_SUCCESS;
} catch (...) {
  return hipObj::handleException();
}

hipObjError_t hipObjParseRdmaReply(const char* reply, size_t replyLen,
                                   int* httpCode) try {
  if (!reply || !httpCode) {
    return {hipObjInvalidValue, 0};
  }
  int code = 0;
  if (!hipObj::parseRdmaReplyHttpCode(reply, replyLen, code)) {
    return {hipObjInvalidValue, 0};
  }
  *httpCode = code;
  return HIPOBJ_SUCCESS;
} catch (...) {
  return hipObj::handleException();
}

hipObjError_t hipObjTokenClientNic(const char* token, char* nicIp,
                                   size_t nicIpLen) try {
  if (!token || !nicIp || nicIpLen == 0) {
    return {hipObjInvalidValue, 0};
  }
#ifdef HIPOBJECT_OFI_API
  if (hipObj::isOfiToken(token)) {
    return hipObj::parseClientNicFromOfiToken(token, nicIp, nicIpLen)
             ? HIPOBJ_SUCCESS
             : hipObjError_t{hipObjInvalidValue, 0};
  }
#endif
  if (!hipObj::parseClientNicFromTokenHex(token, nicIp, nicIpLen)) {
    return {hipObjInvalidValue, 0};
  }
  return HIPOBJ_SUCCESS;
} catch (...) {
  return hipObj::handleException();
}

hipObjError_t hipObjChecksumCrc64Nvme(const void* devPtr, size_t size,
                                      off_t offset,
                                      char out[HIPOBJ_CRC64NVME_B64_SIZE]) try {
  if (!devPtr || !out || offset < 0) {
    return {hipObjInvalidValue, 0};
  }
  uint64_t crc = 0;
  if (hipObj::bufferCrc64nvme(static_cast<const char*>(devPtr) + offset, size,
                              crc) != 0) {
    return {hipObjInternalError, 0};
  }
  const std::string armored = hipObj::armorCrc64nvme(crc);
  std::memcpy(out, armored.c_str(), armored.size() + 1);
  return HIPOBJ_SUCCESS;
} catch (...) {
  return hipObj::handleException();
}

hipObjError_t hipObjVerifyCrc64Nvme(const void* devPtr, size_t size,
                                    off_t offset, const char* header) try {
  if (!devPtr || !header || offset < 0) {
    return {hipObjInvalidValue, 0};
  }
  uint64_t expected = 0;
  if (!hipObj::parseCrc64nvmeHeader(header, expected)) {
    return {hipObjInvalidValue, 0};
  }
  uint64_t actual = 0;
  if (hipObj::bufferCrc64nvme(static_cast<const char*>(devPtr) + offset, size,
                              actual) != 0) {
    return {hipObjInternalError, 0};
  }
  if (actual != expected) {
    fprintf(stderr,
            "hipObj: CRC64NVME mismatch over %zu bytes: the server sent %s, "
            "the buffer holds %s\n",
            size, hipObj::armorCrc64nvme(expected).c_str(),
            hipObj::armorCrc64nvme(actual).c_str());
    return {hipObjChecksumMismatch, 0};
  }
  return HIPOBJ_SUCCESS;
} catch (...) {
  return hipObj::handleException();
}

const char* hipObjGetVersionString(void) try {
  static char buf[32];
  snprintf(buf, sizeof(buf), "%d.%d.%d", HIPOBJ_VERSION_MAJOR,
           HIPOBJ_VERSION_MINOR, HIPOBJ_VERSION_PATCH);
  return buf;
} catch (...) {
  return "0.0.0";
}

// Not yet implemented — callers fall back to the v1 RDMA path.
hipObjError_t hipObjPutV2(const char*, const char*, const void*, uint64_t,
                          uint64_t, const char*, hipObjOpsV2_t*, void*) {
  return {hipObjNotSupported, 0};
}

hipObjError_t hipObjGetV2(const char*, const char*, void*, uint64_t, uint64_t,
                          const char*, hipObjOpsV2_t*, void*) {
  return {hipObjNotSupported, 0};
}

} // extern "C"
