/* Copyright (c) 2026 IBM Corporation. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* libfabric transport: registered buffers are ofi-rma windows, and a GET's
 * token is an ofi1 token that names one.
 *
 *   <base hex>:<size hex>:ofi1:<wire>:<endpoint name hex>:<key hex>
 *
 * Any server process that holds the token and speaks the same wire protocol
 * (rxm.1 for verbs;ofi_rxm, uet.<version> for any UET provider) writes into
 * the window, with no connection to this client: Ceph's OSDs each write
 * their own stripes of a GET. An endpoint thread polls the provider, so the
 * writes land while the HTTP request is open, also on providers that
 * progress manually. */

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "hipobj.h"

namespace ofi_rma {
class Endpoint;
}

namespace hipObj {

class OfiTransport {
public:
  OfiTransport();
  ~OfiTransport();

  OfiTransport(const OfiTransport&) = delete;
  OfiTransport& operator=(const OfiTransport&) = delete;

  /* Open the endpoint. It asks the provider for FI_HMEM when gpuDevice is a
   * device, so that GPU buffers can be lent directly. 0, or -1 with *err
   * set. */
  int open(const hipObjOfiConfig_t& cfg, int gpuDevice, std::string* err);
  /* Deregister every buffer and close the endpoint. */
  void close();
  bool isOpen() const;

  /* Register [ptr, ptr + size). Host memory, and device memory the provider
   * can reach, become windows directly; other device memory is staged
   * through a host buffer, unless HIPOBJ_REQUIRE_GPU_DIRECT is set. */
  int registerBuffer(void* ptr, size_t size, bool hostMemory);
  int deregisterBuffer(void* ptr);
  bool isRegistered(void* ptr) const;
  size_t lookupSize(void* ptr) const;
  /* The host staging buffer behind ptr, or null when the window is ptr. */
  void* lookupHostBuf(void* ptr) const;
  /* Device memory lent without a staging buffer. */
  bool isDirectDevice(void* ptr) const;

  /* The ofi1 token for [offset, offset + size) of the buffer at ptr, or an
   * empty string. */
  std::string makeToken(void* ptr, size_t size, size_t offset);
  /* Order the caller's reads after the writes the endpoint thread placed. */
  void sync();
  /* After a GET into ptr, or a PUT out of it: give its window a new key, so
   * that a late or duplicate write meant for this GET cannot land in the
   * next one, and a server read meant for this PUT cannot read the next
   * contents. Always after a transfer that did not complete. After one that
   * did, when duplicates are possible: over UET, whose RUDI mode places a
   * retransmitted packet again even after the write completed, or when
   * HIPOBJ_OFI_REKEY=always. Otherwise only when the provider can re-key in
   * place, which is cheap. HIPOBJ_OFI_REKEY=failure limits it to transfers
   * that did not complete. */
  void retire(void* ptr, bool completed);
  /* The provider offers RMA reads: windows take a server's reads, and a PUT
   * can leave its payload in the buffer for the server to pull. */
  bool reads() const;

  /* Provider, fabric, domain and memory modes, for logs. */
  std::string describe() const;

private:
  struct Window {
    uint64_t id = 0;
    char* lent = nullptr; /* the memory the window covers */
    size_t size = 0;
    void* hostBuf = nullptr; /* staging buffer this library owns, or null */
    bool device = false;     /* device memory lent directly */
    int dmabufFd = -1;       /* the dma-buf the window was registered from */
  };

  void deregisterAllLocked();

  std::unique_ptr<ofi_rma::Endpoint> ep_;
  int gpuDevice_ = -1;
  enum class RekeyPolicy { failure, cheap, always };
  RekeyPolicy rekey_ = RekeyPolicy::cheap;
  mutable std::mutex mutex_;
  std::map<uintptr_t, Window> windows_;
};

/* Whether token is an ofi1 token. */
bool isOfiToken(const char* token);

/* The IP address in an ofi1 token's endpoint name, for providers whose names
 * are socket addresses (tcp, verbs;ofi_rxm). true with an empty string for
 * other names, false for a malformed token, as parseClientNicFromTokenHex()
 * does for verbs tokens. */
bool parseClientNicFromOfiToken(const char* token, char* nicIp,
                                size_t nicIpLen);

} // namespace hipObj
