# Changelog for hipObject

## (Unreleased) hipObject 0.1.0

### Added

- A libfabric transport, built with `-DHIPOBJECT_OFI_API=ON`.
  `hipObjInitOfi()` lends registered buffers as libfabric windows and sends
  an ofi1 token with each GET. Any server process that runs the same
  provider can write the data, such as each Ceph OSD that holds a stripe.
  GPU buffers are written directly when the provider offers `FI_HMEM` and
  libfabric has ROCr support, and are staged through host memory otherwise.
  The transport serves GET only: `hipObjPut()` returns the new
  `hipObjOpNotSupported`. After a GET that fails, the buffer's window gets a
  new key, so that a late write cannot land in it.

### Changed

- hipObject is now built as plain C/C++ instead of HIP. It contains no GPU
  kernels, so the HIP language (and its per-architecture device compilation
  pass) is no longer enabled. The HIP runtime is still used via the
  `hip::host` CMake target. `CMAKE_HIP_COMPILER` no longer needs to be set
  when configuring.

### Removed

- The `OFFLOAD_ARCH` CMake cache variable and its `rocminfo`-based GPU
  architecture detection. It was never used to set the target architecture
  and is unnecessary now that nothing is compiled for the GPU.

### Known issues

