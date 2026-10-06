/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>
#include <stdlib.h>

#include <sys/types.h>

#if defined(__GNUC__)
#define HIPOBJ_API __attribute__((visibility("default")))
#else
#define HIPOBJ_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file
 *
 * @mainpage hipObject API Reference
 *
 * @section contents Contents
 * - @ref core
 * - @ref error
 * - @ref buffer
 * - @ref io
 */

/*!
 * @defgroup core Core Functionality
 * @defgroup error Errors and Error Handling
 * @defgroup buffer Buffer Registration
 * @defgroup io Data Transfer (GET / PUT)
 */

/* -------------------------------------------------------
 *  LIBRARY VERSION NUMBERS
 * ------------------------------------------------------- */

/*! @brief hipObject major version number @ingroup core */
#define HIPOBJ_VERSION_MAJOR 0
/*! @brief hipObject minor version number @ingroup core */
#define HIPOBJ_VERSION_MINOR 1
/*! @brief hipObject patch version number @ingroup core */
#define HIPOBJ_VERSION_PATCH 0

/* -------------------------------------------------------
 *  ERROR HANDLING
 * ------------------------------------------------------- */

/*! @brief Base value for hipObject error codes @ingroup error */
#define HIPOBJ_BASE_ERR 6000

/*!
 * @brief Operation-level error codes
 * @ingroup error
 */
typedef enum {
  hipObjSuccess = 0,
  hipObjInvalidValue,
  hipObjNotInitialized,
  hipObjAlreadyInitialized,
  hipObjRdmaError,
  hipObjS3Error,
  hipObjBufNotRegistered,
  hipObjBufAlreadyRegistered,
  hipObjNicNotFound,
  hipObjDmabufNotSupported,
  hipObjSizeTooLarge,
  hipObjInternalError,
#ifdef HIPOBJECT_V2_API
  hipObjNotSupported, /*!< Server explicitly does not support hipobj-rc-v2 */
  hipObjBusy,         /*!< Server backpressure (503) and retries exhausted */
#endif
#ifdef HIPOBJECT_OFI_API
  hipObjOpNotSupported, /*!< The active transport cannot do this operation */
#endif
  hipObjChecksumMismatch, /*!< The bytes do not match the checksum */
} hipObjOpError_t;

/*!
 * @brief Compound error type carrying both an
 *        operation error and an optional HIP error
 * @ingroup error
 */
typedef struct {
  hipObjOpError_t opError;
  int hipError;
} hipObjError_t;

#define HIPOBJ_SUCCESS ((hipObjError_t){hipObjSuccess, 0})

/*!
 * @brief Return a human-readable string for an
 *        operation error code
 * @ingroup error
 */
HIPOBJ_API const char* hipObjGetErrorString(hipObjOpError_t err);

/* -------------------------------------------------------
 *  TYPES
 * ------------------------------------------------------- */

/*!
 * @brief Opaque handle to an S3 object for RDMA I/O
 * @ingroup core
 */
typedef void* hipObjHandle_t;

/*!
 * @brief Callback struct for S3 SDK integration.
 *
 * The application provides these callbacks so that
 * hipObject can inject RDMA tokens into S3 requests
 * and receive RDMA reply tags from the server.  This
 * mirrors cuObject's CUObjOps_t pattern.
 *
 * @ingroup core
 */
typedef struct {
  /*!
   * Called by hipObject to send the RDMA token.
   * The application should embed the token in the
   * x-amz-rdma-token S3 request header.
   *
   * @param ctx       User-supplied context pointer
   * @param token     Hex-encoded RDMA token
   * @param tokenLen  Length of token in bytes
   * @return 0 on success, negative on failure
   */
  int (*sendRequest)(void* ctx, const char* token, size_t tokenLen);

  /*!
   * Called by hipObject to receive the RDMA reply.
   * The application should extract x-amz-rdma-reply
   * from the S3 response headers.
   *
   * @param ctx       User-supplied context pointer
   * @param reply     Buffer to receive the reply tag
   * @param replyLen  In: buffer size; out: actual length
   * @return 0 on success, negative on failure
   */
  int (*recvReply)(void* ctx, char* reply, size_t* replyLen);
} hipObjOps_t;

/*!
 * @brief Configuration for hipObject initialization
 * @ingroup core
 */
typedef struct {
  const char* endpoint;  /*!< S3 endpoint URL           */
  const char* region;    /*!< AWS region                */
  const char* accessKey; /*!< S3 access key (optional)  */
  const char* secretKey; /*!< S3 secret key (optional)  */
  uint32_t flags;        /*!< Reserved, set to 0        */
  int gpuDevice;         /*!< HIP device index, or -1   */
  const char* nicHint;   /*!< NIC name hint, or NULL    */
} hipObjConfig_t;

/*! @brief RDMA token operation: server RDMA READ (client PUT) @ingroup io */
#define HIPOBJ_RDMA_OP_PUT 0
/*! @brief RDMA token operation: server RDMA WRITE (client GET) @ingroup io */
#define HIPOBJ_RDMA_OP_GET 1

/*! @brief x-amz-rdma-reply: server declined RDMA (HTTP fallback) @ingroup io */
#define HIPOBJ_RDMA_REPLY_NOT_IMPLEMENTED 501

/* -------------------------------------------------------
 *  CORE LIFECYCLE
 * ------------------------------------------------------- */

/*!
 * @brief Initialize the hipObject library
 *
 * Opens the RDMA device, selects the closest NIC to
 * the GPU, and prepares internal state.  Must be
 * called before any other hipObj* function.
 *
 * @param config  Pointer to configuration struct
 * @return hipObjError_t
 * @ingroup core
 */
HIPOBJ_API hipObjError_t hipObjInit(hipObjConfig_t* config);

/*!
 * @brief Shut down the hipObject library
 *
 * Releases all RDMA resources.  All registered
 * buffers are implicitly deregistered.
 *
 * @return hipObjError_t
 * @ingroup core
 */
HIPOBJ_API hipObjError_t hipObjShutdown(void);

/* -------------------------------------------------------
 *  BUFFER REGISTRATION
 * ------------------------------------------------------- */

/*!
 * @brief Register a GPU buffer for RDMA transfers
 *
 * Exports the GPU buffer via dmabuf and registers it
 * with the RDMA NIC.  Maximum 4 GiB per registration.
 *
 * @param devPtr  Pointer returned by hipMalloc
 * @param size    Size of the buffer in bytes
 * @return hipObjError_t
 * @ingroup buffer
 */
HIPOBJ_API hipObjError_t hipObjBufRegister(void* devPtr, size_t size);

/*!
 * @brief Register a host buffer for RDMA transfers
 *
 * Registers user-owned CPU RAM, such as a buffer allocated
 * with malloc() or hipHostMalloc(), with the RDMA NIC.
 * hipObject does not allocate or free the host buffer.
 * Maximum 4 GiB per registration.
 *
 * @param hostPtr  Pointer to CPU-accessible memory
 * @param size     Size of the buffer in bytes
 * @return hipObjError_t
 * @ingroup buffer
 */
HIPOBJ_API hipObjError_t hipObjBufRegisterHost(void* hostPtr, size_t size);

/*!
 * @brief Deregister a previously registered buffer
 *
 * @param devPtr  Pointer previously passed to
 *                hipObjBufRegister or hipObjBufRegisterHost
 * @return hipObjError_t
 * @ingroup buffer
 */
HIPOBJ_API hipObjError_t hipObjBufDeregister(void* devPtr);

/* -------------------------------------------------------
 *  DATA TRANSFER
 * ------------------------------------------------------- */

/*!
 * @brief GET: fetch an S3 object into a registered
 *        buffer via RDMA
 *
 * The server performs an RDMA WRITE to push data into
 * the registered buffer.
 *
 * @param handle  S3 object handle (from application)
 * @param devPtr  Registered GPU or host buffer
 * @param size    Number of bytes to transfer
 * @param offset  Byte offset into the S3 object
 * @param ops     S3 SDK callbacks
 * @param ctx     User context passed to callbacks
 * @return hipObjError_t
 * @ingroup io
 */
HIPOBJ_API hipObjError_t hipObjGet(hipObjHandle_t handle, void* devPtr,
                                   size_t size, off_t offset, hipObjOps_t* ops,
                                   void* ctx);

/*!
 * @brief PUT: store a registered buffer to an S3 object
 *        via RDMA
 *
 * The server performs an RDMA READ to pull data from
 * the registered buffer.
 *
 * @param handle  S3 object handle (from application)
 * @param devPtr  Registered GPU or host buffer
 * @param size    Number of bytes to transfer
 * @param offset  Byte offset into the S3 object
 * @param ops     S3 SDK callbacks
 * @param ctx     User context passed to callbacks
 * @return hipObjError_t
 * @ingroup io
 */
HIPOBJ_API hipObjError_t hipObjPut(hipObjHandle_t handle, const void* devPtr,
                                   size_t size, off_t offset, hipObjOps_t* ops,
                                   void* ctx);

/* -------------------------------------------------------
 *  LIBFABRIC TRANSPORT (ofi1 TOKENS)
 * ------------------------------------------------------- */

#ifdef HIPOBJECT_OFI_API

/*!
 * @brief Settings of the libfabric transport
 * @ingroup core
 *
 * The provider must be the one the server's writers run: for Ceph, the
 * OSDs' osd_ofi_provider. The strings are copied.
 */
typedef struct {
  const char* provider; /*!< Required: "tcp", "verbs;ofi_rxm", "uet", ...  */
  const char* domain;   /*!< RDMA device or interface, or NULL             */
  const char* node;     /*!< Local address to bind, or NULL                */
  const char* service;  /*!< Local port to bind, or NULL                   */
} hipObjOfiConfig_t;

/*!
 * @brief Initialize hipObject with the libfabric transport
 *
 * Use this in place of hipObjInit(). Registered buffers become libfabric
 * windows, and a GET sends an ofi1 token that names the window:
 *
 *   <base hex>:<size hex>:ofi1:<provider>:<endpoint name hex>:<key hex>
 *
 * Any server process that holds the token and runs the same provider writes
 * the object straight into the buffer, with no connection to the client. Ceph
 * forwards the token to its OSDs, so each OSD writes its own stripes. A GPU
 * buffer is written directly when the provider offers FI_HMEM and libfabric
 * has ROCr support; otherwise it is staged through host memory, as with
 * hipObjInit(), unless HIPOBJ_REQUIRE_GPU_DIRECT is set.
 *
 * The transport serves GET only: hipObjPut() and a PUT token from
 * hipObjGetRdmaToken() return hipObjOpNotSupported. Send a PUT's payload
 * over HTTP. hipObjBufSync() also orders the caller's reads after the
 * writes the transport placed, so call it after a GET made with
 * hipObjGetRdmaToken(), even for a buffer that is not staged.
 *
 * After a GET that failed, the buffer's window gets a new key, so a write
 * that arrives late cannot land in it. Over UET it gets one after every
 * GET: RUDI can place a retransmitted packet again after the write that
 * carried it completed. HIPOBJ_OFI_REKEY=always or =failure overrides.
 *
 * @param config  Common settings, as for hipObjInit(); nicHint is unused
 * @param ofi     libfabric settings
 * @return hipObjError_t
 * @ingroup core
 */
HIPOBJ_API hipObjError_t hipObjInitOfi(hipObjConfig_t* config,
                                       const hipObjOfiConfig_t* ofi);

#endif /* HIPOBJECT_OFI_API */

/* -------------------------------------------------------
 *  hipobj-rc-v2 (TWO-ROUND-TRIP CONTROL PROTOCOL)
 * ------------------------------------------------------- */

#ifdef HIPOBJECT_V2_API

/*!
 * @brief V2 control endpoint settings
 * @ingroup core
 *
 * The v2 protocol runs its prepare/ready/cancel exchange on a dedicated
 * control endpoint; there is no default port, the caller must supply one.
 */
typedef struct {
  const char* controlEndpoint; /*!< Required "http(s)://host:port" URI;
                                    the library copies the string */
} hipObjControlEndpointV2_t;

/*!
 * @brief V2 initialization configuration
 * @ingroup core
 */
typedef struct {
  hipObjConfig_t v1;                 /*!< All v1 fields */
  hipObjControlEndpointV2_t control; /*!< v2 control endpoint (required) */
} hipObjConfigV2_t;

/* Forward declaration of the phase-aware callback set (see below). */
typedef struct hipObjOpsV2 hipObjOpsV2_t;

/*!
 * @brief Per-transfer request description for the v2 phases
 * @ingroup io
 *
 * Borrow contract: string fields are owned by the library and remain
 * valid for the duration of a single callback invocation. Callbacks are
 * synchronous (they return before the library continues) and must copy
 * anything they need to keep.
 */
typedef struct {
  const char* method;  /*!< "GET" or "PUT" */
  const char* bucket;  /*!< Object bucket */
  const char* key;     /*!< Object key */
  const char* query;   /*!< Canonical query string or NULL */
  const char* token;   /*!< 88-hex RDMA token */
  const char* session; /*!< READY/cancel: session id (library sets) */
  const char* target;  /*!< Canonical rdma-target value (library sets) */
  uint64_t size;       /*!< Transfer size in bytes */
  uint64_t offset;     /*!< Byte offset into the object */
  uint32_t cookie;     /*!< Client cookie (library generates) */
  uint32_t clientPsn;  /*!< Client PSN, 1..0xffffff (library generates) */
  const hipObjControlEndpointV2_t* endpoint; /*!< Control endpoint (library sets
                                                from init) */
} hipObjTransferReqV2_t;

/*! @brief Response to PREPARE @ingroup io */
typedef struct {
  int httpStatus;        /*!< Status code (200/501/403/413/503/500) */
  int protocolEcho;      /*!< 1 when X-Amz-Rdma-Protocol: hipobj-rc-v2 seen */
  int unsupportedMarker; /*!< 1 when the explicit unsupported marker seen */
  char serverToken[97];  /*!< 88-hex peer token + NUL */
  char session[65];      /*!< 32-hex session id + NUL */
  uint32_t serverPsn;    /*!< Server PSN, 1..0xffffff (0 = invalid) */
} hipObjPrepareReplyV2_t;

/*! @brief FINAL response (the reply to READY) @ingroup io */
typedef struct {
  int httpStatus;       /*!< 200 (GET) / 204 (PUT) / 5xx / 409 / 408 */
  int protocolEcho;     /*!< 1 when the protocol echo header was present */
  uint64_t bytes;       /*!< Bytes transferred per the server */
  uint32_t cookieEcho;  /*!< Must match the request cookie */
  char etag[128];       /*!< S3 ETag when present, else empty */
  char versionId[128];  /*!< S3 version id when present, else empty */
  char checksumB64[13]; /*!< 12-char canonical CRC64NVME base64 or empty */
  int cookiePresent;    /*!< 1 when the cookie echo header was present */
} hipObjFinalReplyV2_t;

/*!
 * @brief Phase-aware callbacks for the v2 control protocol
 * @ingroup io
 *
 * Each send* callback performs one complete HTTP round trip on the
 * control endpoint and fills @p out from the response. All callbacks are
 * required for v2 transfers. The v1 member is unused by the v2 entry
 * points and is kept for structural forward compatibility.
 */
typedef struct hipObjOpsV2 {
  hipObjOps_t v1;

  /*! Issue PREPARE; out is filled from the response headers. */
  int (*sendPrepare)(void* ctx, const hipObjTransferReqV2_t* req,
                     hipObjPrepareReplyV2_t* out);

  /*! Issue READY; the response is FINAL. out reflects it. */
  int (*sendReady)(void* ctx, const hipObjTransferReqV2_t* req,
                   hipObjFinalReplyV2_t* out);

  /*! Issue CANCEL (idempotent). Only the HTTP status matters. */
  int (*sendCancel)(void* ctx, const hipObjTransferReqV2_t* req);
} hipObjOpsV2_t;

/*!
 * @brief Initialize the library for hipobj-rc-v2 transfers
 * @ingroup core
 *
 * Mutually exclusive with hipObjInit: whichever is called first wins and
 * the other returns hipObjAlreadyInitialized until hipObjShutdown.
 */
HIPOBJ_API hipObjError_t hipObjInitV2(hipObjConfigV2_t* config);

/*!
 * @brief V2 GET: download an object into a registered buffer
 * @ingroup io
 *
 * Runs the two-round-trip protocol (PREPARE, then READY whose response
 * is FINAL) against the configured control endpoint. The transfer size
 * is capped at 2^31-1 bytes; larger requests fail with
 * hipObjSizeTooLarge. When the server answers PREPARE with 501 plus the
 * protocol-unsupported marker the function returns hipObjNotSupported
 * and the caller may fall back to plain HTTP; failures after READY are
 * never retried or fallen back. The session lifetime is the function
 * scope: on return the session is terminated and the connection
 * quiesced.
 */
HIPOBJ_API hipObjError_t hipObjGetV2(const char* bucket, const char* key,
                                     void* devPtr, uint64_t size,
                                     uint64_t offset, const char* query,
                                     hipObjOpsV2_t* ops, void* ctx);

/*! @brief V2 PUT, same contract as hipObjGetV2 @ingroup io */
HIPOBJ_API hipObjError_t hipObjPutV2(const char* bucket, const char* key,
                                     const void* devPtr, uint64_t size,
                                     uint64_t offset, const char* query,
                                     hipObjOpsV2_t* ops, void* ctx);

#endif /* HIPOBJECT_V2_API */

/*! @brief hipObjBufSync direction: device -> staging buffer @ingroup io */
#define HIPOBJ_SYNC_TO_HOST 0
/*! @brief hipObjBufSync direction: staging buffer -> device @ingroup io */
#define HIPOBJ_SYNC_TO_DEVICE 1

/*!
 * @brief Stage a registered buffer between device and host memory
 *
 * When a device pointer cannot be registered with the NIC directly (no
 * dmabuf support, or a fabric with no peer-to-peer path to the GPU),
 * hipObjBufRegister() registers a host staging buffer instead and the
 * peer's RDMA reads and writes land there. Callers driving a transfer
 * through hipObjGetRdmaToken() must therefore call this with
 * HIPOBJ_SYNC_TO_HOST before a PUT and HIPOBJ_SYNC_TO_DEVICE after a
 * GET. It is a no-op, reporting success, for a directly registered
 * buffer; hipObjGet()/hipObjPut() and the V2 entry points do it
 * themselves.
 *
 * @ingroup io
 */
HIPOBJ_API hipObjError_t hipObjBufSync(void* devPtr, size_t size, off_t offset,
                                       int direction);

/*!
 * @brief Mint a hex-encoded RC RDMA token for a registered buffer
 *
 * The caller must release @p *outToken with hipObjPutRdmaToken().
 * @p op is HIPOBJ_RDMA_OP_PUT or HIPOBJ_RDMA_OP_GET (reserved).
 *
 * @ingroup io
 */
HIPOBJ_API hipObjError_t hipObjGetRdmaToken(const void* devPtr, size_t size,
                                            int op, char** outToken);

/*!
 * @brief Mint a token for part of a registered buffer
 * Like hipObjGetRdmaToken(), for [devPtr + offset, devPtr + offset + size).
 * devPtr must be the address that was registered. Several tokens for
 * different parts of one buffer can be in use at the same time, so one
 * registration serves many concurrent ranged GETs. A range that does not
 * fit in the registration returns hipObjInvalidValue. After a GET, call
 * hipObjBufSync() with the same size and offset.
 * @ingroup io
 */
HIPOBJ_API hipObjError_t hipObjGetRdmaTokenAt(const void* devPtr, size_t size,
                                              size_t offset, int op,
                                              char** outToken);

/*!
 * @brief Release a token allocated by hipObjGetRdmaToken()
 * @ingroup io
 */
HIPOBJ_API hipObjError_t hipObjPutRdmaToken(char* token);

/*!
 * @brief Parse an x-amz-rdma-reply header value
 *
 * On success writes the HTTP-style reply code (200, 204, 206, 501)
 * to @p httpCode.
 *
 * @ingroup io
 */
HIPOBJ_API hipObjError_t hipObjParseRdmaReply(const char* reply,
                                              size_t replyLen, int* httpCode);

/*!
 * @brief Extract client NIC IPv4 from a minted RDMA token
 *
 * Writes a dotted-quad address into @p nicIp when the token GID
 * carries an IPv4-mapped RoCEv2 suffix.  Returns hipObjSuccess with
 * an empty string when no address is encoded.
 *
 * @ingroup io
 */
HIPOBJ_API hipObjError_t hipObjTokenClientNic(const char* token, char* nicIp,
                                              size_t nicIpLen);

/* -------------------------------------------------------
 *  CHECKSUMS (CRC-64/NVME)
 * ------------------------------------------------------- */

/*! @brief Size of a CRC64NVME checksum in S3's base64 form, with the NUL
 *  @ingroup io */
#define HIPOBJ_CRC64NVME_B64_SIZE 13

/*!
 * @brief Compute the CRC64NVME checksum of a buffer, as S3 renders it
 *
 * Writes the 12-character base64 of the big-endian CRC-64/NVME of
 * [devPtr + offset, devPtr + offset + size): the value of an
 * x-amz-checksum-crc64nvme header. Send it with a PUT, and the server
 * rejects an upload whose bytes do not match. Device memory is read
 * through the host, so this costs a copy of the range.
 *
 * @param devPtr  GPU or host buffer
 * @param size    Number of bytes to checksum
 * @param offset  Byte offset into the buffer
 * @param out     Receives the base64 text and a NUL
 * @return hipObjError_t
 * @ingroup io
 */
HIPOBJ_API hipObjError_t
hipObjChecksumCrc64Nvme(const void* devPtr, size_t size, off_t offset,
                        char out[HIPOBJ_CRC64NVME_B64_SIZE]);

/*!
 * @brief Verify a buffer against a CRC64NVME checksum header
 *
 * Accepts the value of either header:
 * - x-amz-rdma-checksum: "CRC64NVME <base64>". Ceph sends it with an
 *   out-of-band GET, computed by the storage nodes from the bytes they
 *   placed. It covers exactly the bytes delivered, so it also checks a
 *   ranged GET.
 * - x-amz-checksum-crc64nvme: "<base64>". S3 sends it for a GET with
 *   x-amz-checksum-mode: ENABLED and no range. It covers the whole object,
 *   so pass it only when the GET read the whole object.
 *
 * @param devPtr  GPU or host buffer the GET filled
 * @param size    Number of bytes the GET delivered
 * @param offset  Byte offset into the buffer where they start
 * @param header  The header's value
 * @return hipObjSuccess when the bytes match, hipObjChecksumMismatch when
 *         they do not, hipObjInvalidValue for a malformed or composite
 *         (multipart) value
 * @ingroup io
 */
HIPOBJ_API hipObjError_t hipObjVerifyCrc64Nvme(const void* devPtr, size_t size,
                                               off_t offset,
                                               const char* header);

/*!
 * @brief Return the library version as a string
 * @ingroup core
 */
HIPOBJ_API const char* hipObjGetVersionString(void);

#ifdef __cplusplus
}
#endif
