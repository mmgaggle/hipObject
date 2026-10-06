# Copyright (c) 2026 IBM Corporation. All rights reserved.
#
# SPDX-License-Identifier: MIT

# ofi-rma: libfabric windows and ofi1 tokens, for the libfabric transport
# (HIPOBJECT_OFI_API). An installed ofi-rma is used when find_package()
# finds one; otherwise the source is fetched. To build from a local
# checkout, set FETCHCONTENT_SOURCE_DIR_OFI_RMA to its path.

include_guard(GLOBAL)

set(HIPOBJ_OFI_RMA_GIT_REPOSITORY
  "https://github.com/mmgaggle/ofi-rma.git"
  CACHE STRING "ofi-rma repository to fetch")
set(HIPOBJ_OFI_RMA_GIT_TAG
  "3f0083edb6edcd47940f0c522678495514f45a85"
  CACHE STRING "ofi-rma revision to fetch")

find_package(ofi_rma 0.1 CONFIG QUIET)
if(ofi_rma_FOUND)
  message(STATUS "ofi-rma found @ ${ofi_rma_DIR}")
else()
  include(FetchContent)
  # A static library inside libhipobj, installed with it: the exported
  # hipobj targets name ofi_rma::ofi_rma, so consumers find it next to them.
  set(OFI_RMA_SHARED OFF CACHE BOOL "" FORCE)
  set(OFI_RMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(OFI_RMA_INSTALL ON CACHE BOOL "" FORCE)
  FetchContent_Declare(
    ofi_rma
    GIT_REPOSITORY ${HIPOBJ_OFI_RMA_GIT_REPOSITORY}
    GIT_TAG ${HIPOBJ_OFI_RMA_GIT_TAG}
  )
  FetchContent_MakeAvailable(ofi_rma)
  message(STATUS "ofi-rma built from ${ofi_rma_SOURCE_DIR}")
endif()
