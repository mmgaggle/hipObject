/* Copyright (c) 2026 IBM Corporation. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "ofi-transport.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <hip/hip_runtime.h>
#include <hsa/hsa_ext_amd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <ofi_rma/ofi_rma.h>
#include <sys/socket.h>
#include <unistd.h>

#include "buffer.h"
#include "hip-seam.h"

namespace hipObj {

OfiTransport::OfiTransport() = default;

/* Process exit, without hipObjShutdown(): close the endpoint, which closes
 * its regions, but leave staging buffers alone, since the HIP runtime may
 * already be gone. */
OfiTransport::~OfiTransport() {
  ep_.reset();
}

int OfiTransport::open(const hipObjOfiConfig_t& cfg, int gpuDevice,
                       std::string* err) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (ep_) {
    *err = "the libfabric endpoint is already open";
    return -1;
  }
  if (!cfg.provider || !*cfg.provider) {
    *err = "no libfabric provider named";
    return -1;
  }
  ofi_rma::config_t c;
  c.provider = cfg.provider;
  c.domain = cfg.domain ? cfg.domain : "";
  c.node = cfg.node ? cfg.node : "";
  c.service = cfg.service ? cfg.service : "";
  /* The client waits in an HTTP request while the server writes; a provider
   * that progresses manually places the writes only while it is polled. */
  c.progress_thread = true;
  c.hmem = gpuDevice >= 0;
  ep_ = ofi_rma::Endpoint::open(c, err);
  if (!ep_) {
    return -1;
  }
  gpuDevice_ = gpuDevice;
  /* UET's RUDI mode retransmits without connection state, so a duplicate
   * of a completed GET's write can arrive during the next GET. */
  const char* policy = getenv("HIPOBJ_OFI_REKEY");
  if (policy && std::strcmp(policy, "always") == 0) {
    rekey_ = RekeyPolicy::always;
  } else if (policy && std::strcmp(policy, "failure") == 0) {
    rekey_ = RekeyPolicy::failure;
  } else {
    rekey_ = ep_->provider().rfind("uet", 0) == 0 ? RekeyPolicy::always
                                                  : RekeyPolicy::cheap;
  }
  return 0;
}

void OfiTransport::close() {
  std::lock_guard<std::mutex> lock(mutex_);
  deregisterAllLocked();
  ep_.reset();
  gpuDevice_ = -1;
}

bool OfiTransport::isOpen() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return ep_ != nullptr;
}

int OfiTransport::registerBuffer(void* ptr, size_t size, bool hostMemory) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!ep_ || !ptr || size == 0) {
    return -1;
  }
  const uintptr_t key = reinterpret_cast<uintptr_t>(ptr);
  if (validateRegistration(windows_.count(key) != 0, windows_.size(), size) !=
      0) {
    return -1;
  }
  char* mem = static_cast<char*>(ptr);
  ofi_rma::Endpoint::window_t w;
  if (hostMemory || !isDevicePointer(ptr)) {
    if (ep_->register_window(mem, size, &w) < 0) {
      fprintf(stderr,
              "hipObj: libfabric registration of %zu bytes at %p failed: "
              "%s\n",
              size, ptr, ep_->last_error().c_str());
      return -1;
    }
    windows_[key] = {w.id, mem, size, nullptr, false};
    return 0;
  }

  /* Device memory: lend it directly when the provider offers FI_HMEM. First
   * as a dma-buf that ROCr exports, the way the verbs path registers it,
   * which needs nothing of libfabric's own export: that needs a kernel built
   * with CONFIG_DMABUF_MOVE_NOTIFY. Then through libfabric's FI_HMEM. A
   * provider without FI_HMEM can accept a dma-buf (tcp does) and still not
   * place a write into it. */
  std::string why = "the provider offers no FI_HMEM";
  if (ep_->hmem()) {
    ofi_rma::memory_t dev;
    dev.iface = ofi_rma::memory_t::iface_t::rocr;
    dev.device = gpuDevice_ < 0 ? 0 : gpuDevice_;
    int fd = -1;
    uint64_t fdOffset = 0;
    if (hsa_amd_portable_export_dmabuf(ptr, size, &fd, &fdOffset) ==
        HSA_STATUS_SUCCESS) {
      ofi_rma::memory_t dmabuf = dev;
      dmabuf.dmabuf_fd = fd;
      dmabuf.dmabuf_offset = fdOffset;
      if (ep_->register_window(mem, size, dmabuf, &w) == 0) {
        windows_[key] = {w.id, mem, size, nullptr, true, fd};
        return 0;
      }
      why = ep_->last_error();
      ::close(fd);
    }
    if (ep_->register_window(mem, size, dev, &w) == 0) {
      windows_[key] = {w.id, mem, size, nullptr, true};
      return 0;
    }
    why = ep_->last_error();
  }
  if (requireGpuDirect()) {
    fprintf(stderr,
            "hipObj: GPU-direct libfabric registration of %zu bytes at %p "
            "failed (%s) and HIPOBJ_REQUIRE_GPU_DIRECT is set; refusing to "
            "stage through host memory.\n",
            size, ptr, why.c_str());
    return -1;
  }
  fprintf(stderr,
          "hipObj: GPU-direct libfabric registration of %zu bytes at %p "
          "failed (%s); falling back to a host staging buffer. This transfer "
          "is no longer GPU-direct.\n",
          size, ptr, why.c_str());

  void* hostBuf = nullptr;
  hipError_t err = hipOps().hipHostMalloc(&hostBuf, size, hipHostMallocDefault);
  if (err != hipSuccess || !hostBuf) {
    return -1;
  }
  if (ep_->register_window(static_cast<char*>(hostBuf), size, &w) < 0) {
    fprintf(stderr,
            "hipObj: libfabric registration of a %zu-byte staging buffer "
            "failed: %s\n",
            size, ep_->last_error().c_str());
    freeOwnedHostBuffer(hostBuf);
    return -1;
  }
  windows_[key] = {w.id, static_cast<char*>(hostBuf), size, hostBuf, false};
  return 0;
}

int OfiTransport::deregisterBuffer(void* ptr) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = windows_.find(reinterpret_cast<uintptr_t>(ptr));
  if (it == windows_.end() || !ep_) {
    return -1;
  }
  ep_->deregister_window(it->second.id);
  freeOwnedHostBuffer(it->second.hostBuf);
  if (it->second.dmabufFd >= 0) {
    ::close(it->second.dmabufFd);
  }
  windows_.erase(it);
  return 0;
}

void OfiTransport::deregisterAllLocked() {
  for (auto& [key, w] : windows_) {
    if (ep_) {
      ep_->deregister_window(w.id);
    }
    freeOwnedHostBuffer(w.hostBuf);
    if (w.dmabufFd >= 0) {
      ::close(w.dmabufFd);
    }
  }
  windows_.clear();
}

bool OfiTransport::isRegistered(void* ptr) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return windows_.count(reinterpret_cast<uintptr_t>(ptr)) != 0;
}

size_t OfiTransport::lookupSize(void* ptr) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = windows_.find(reinterpret_cast<uintptr_t>(ptr));
  return it == windows_.end() ? 0 : it->second.size;
}

void* OfiTransport::lookupHostBuf(void* ptr) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = windows_.find(reinterpret_cast<uintptr_t>(ptr));
  return it == windows_.end() ? nullptr : it->second.hostBuf;
}

bool OfiTransport::isDirectDevice(void* ptr) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = windows_.find(reinterpret_cast<uintptr_t>(ptr));
  return it != windows_.end() && it->second.device;
}

std::string OfiTransport::makeToken(void* ptr, size_t size, size_t offset) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = windows_.find(reinterpret_cast<uintptr_t>(ptr));
  if (it == windows_.end() || !ep_) {
    return {};
  }
  const Window& w = it->second;
  if (size == 0 || offset > w.size || size > w.size - offset) {
    return {};
  }
  const ofi_rma::Endpoint::window_t win{w.id, w.lent, w.size};
  std::string token = ep_->window_token(win, offset, size);
  if (token.empty()) {
    fprintf(stderr, "hipObj: the libfabric endpoint gave no token: %s\n",
            ep_->last_error().c_str());
  }
  return token;
}

void OfiTransport::sync() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (ep_) {
    ep_->sync();
  }
}

void OfiTransport::retire(void* ptr, bool completed) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = windows_.find(reinterpret_cast<uintptr_t>(ptr));
  if (it == windows_.end() || !ep_) {
    return;
  }
  /* A reliable connected provider delivers each write once, and the server
   * replied only after its writes completed: after a GET that completed,
   * the window is quiet. Re-key it anyway when that is cheap, since an
   * unordered connectionless delivery mode can repeat a write late. */
  if (completed &&
      (rekey_ == RekeyPolicy::failure ||
       (rekey_ == RekeyPolicy::cheap && ep_->stats().rekey_in_place == 0))) {
    return;
  }
  const Window& w = it->second;
  const ofi_rma::Endpoint::window_t win{w.id, w.lent, w.size};
  if (int r = ep_->rekey_window(win); r < 0) {
    fprintf(stderr,
            "hipObj: giving the libfabric window at %p a new key failed "
            "(%d: %s); a late write of the last GET can still land in it, "
            "so do not reuse the buffer for %lld ms.\n",
            ptr, r, ep_->last_error().c_str(),
            static_cast<long long>(ep_->key_quarantine().count()));
  }
}

std::string OfiTransport::describe() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return ep_ ? ep_->describe() : std::string();
}

bool isOfiToken(const char* token) {
  return token && ofi_rma::parse_token(token).has_value();
}

bool parseClientNicFromOfiToken(const char* token, char* nicIp,
                                size_t nicIpLen) {
  if (!nicIp || nicIpLen == 0) {
    return false;
  }
  nicIp[0] = '\0';
  const auto t = token ? ofi_rma::parse_token(token) : std::nullopt;
  if (!t) {
    return false;
  }
  const std::string& name = t->name;
  sa_family_t family = 0;
  if (name.size() < sizeof(family)) {
    return true;
  }
  std::memcpy(&family, name.data(), sizeof(family));
  if (family == AF_INET && name.size() >= sizeof(sockaddr_in)) {
    sockaddr_in sa{};
    std::memcpy(&sa, name.data(), sizeof(sa));
    return inet_ntop(AF_INET, &sa.sin_addr, nicIp,
                     static_cast<socklen_t>(nicIpLen)) != nullptr;
  }
  if (family == AF_INET6 && name.size() >= sizeof(sockaddr_in6)) {
    sockaddr_in6 sa{};
    std::memcpy(&sa, name.data(), sizeof(sa));
    return inet_ntop(AF_INET6, &sa.sin6_addr, nicIp,
                     static_cast<socklen_t>(nicIpLen)) != nullptr;
  }
  return true;
}

} // namespace hipObj
