/**
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @file cs_api.h
 * @brief SNIA Computational Storage API Host Library
 *
 * This header defines the public interface for the CS API Host Library,
 * which provides a standardized interface for computational storage operations.
 *
 * @par Thread safety
 * All API entry points are thread-safe: per-device work is serialized by a
 * per-device mutex, and the library/device/event tables are protected by
 * global mutexes. The library is initialized lazily (cs_ensure_init) on
 * first use, so csInit() needs no explicit ordering with other calls.
 * csShutdown() is process-global and force-closes still-open handles —
 * call it only after all other threads have stopped using the library.
 */

#ifndef CS_API_H
#define CS_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup cs_status Status Codes
 * @{
 */

/** CS API status codes */
typedef enum {
	CS_STATUS_SUCCESS = 0,              /**< Operation completed successfully */
	CS_STATUS_PENDING = 1,              /**< Operation is pending completion */

	/* Error codes (negative values) */
	CS_STATUS_ERROR_INVALID_HANDLE = -1,    /**< Invalid handle provided */
	CS_STATUS_ERROR_INVALID_PARAM = -2,     /**< Invalid parameter */
	CS_STATUS_ERROR_NO_MEMORY = -3,         /**< Memory allocation failed */
	CS_STATUS_ERROR_NOT_FOUND = -4,         /**< Resource not found */
	CS_STATUS_ERROR_BUSY = -5,              /**< Resource is busy */
	CS_STATUS_ERROR_TIMEOUT = -6,           /**< Operation timed out */
	CS_STATUS_ERROR_IO = -7,                /**< I/O error occurred */
	CS_STATUS_ERROR_NOT_SUPPORTED = -8,     /**< Operation not supported */
	CS_STATUS_ERROR_PERMISSION = -9,        /**< Permission denied */
	CS_STATUS_ERROR_NO_DEVICE = -10,        /**< No device available */
	CS_STATUS_ERROR_BACKEND = -11,          /**< Backend-specific error */
	CS_STATUS_ERROR_NOT_INITIALIZED = -12,  /**< Library not initialized */
	CS_STATUS_ERROR_ALREADY_EXISTS = -13,   /**< Resource already exists */
	CS_STATUS_ERROR_LIMIT = -14,            /**< Resource limit reached */
} csStatus;

/**
 * Convert a csStatus code to a human-readable string
 *
 * @param[in] status    Status code to convert
 *
 * @return String representation of the status code
 */
const char *csStatusToString(csStatus status);

/** @} */

/**
 * @defgroup cs_handles Handle Types
 * @{
 */

/** Opaque handle to a Computational Storage Execution unit (CSx) */
typedef uint64_t csCSxHandle;

/** Opaque handle to a memory allocation */
typedef uint64_t csMemHandle;

/** Opaque handle to an event for completion notification */
typedef uint64_t csEventHandle;

/** Computational Storage Function identifier */
typedef uint32_t csCSFId;

/** Built-in CSF IDs for standard compute programs */
#define CS_CSF_SUM64             2U
#define CS_CSF_MAX64             3U
#define CS_CSF_MIN64             4U
#define CS_CSF_DOT_PRODUCT       5U
#define CS_CSF_FILTER_GT         6U
#define CS_CSF_MEMCPY            7U
#define CS_CSF_RLE_COMPRESS      8U
#define CS_CSF_MULTI_AGG64       9U
#define CS_CSF_L2_DISTANCE_SQ   10U
#define CS_CSF_COSINE_SIMILARITY 11U
/*
 * Control-group CSFs. Reduction ratio is 1.0 (output == input), so these must
 * LOSE against the host path in any fair benchmark; they exist to prove the
 * measurement is honest: a CSF that cannot possibly win still shows up as a
 * loss, so wins elsewhere aren't an artifact of a rigged comparison.
 */
#define CS_CSF_MEMFILL          12U
#define CS_CSF_MEMCPY_INLINE    13U
/*
 * Evaluation CSFs. Unlike the others these take a request struct in the args
 * buffer (see the device-side evaluation request header) while input_buffer
 * carries the metadata records to scan, and the result lands in the output
 * buffer rather than being a single scalar.
 */
#define CS_CSF_FILTER_AGG            14U
#define CS_CSF_FILTERED_TOPK_EXACT   15U

/** Invalid handle constant */
#define CS_INVALID_HANDLE ((uint64_t)0)

/** @} */

/**
 * @defgroup cs_memory Memory Types
 * @{
 */

/** Memory allocation types */
typedef enum {
	/**
	 * Application-Facing Device Memory (AFDM)
	 * Host-visible memory that can be used for data transfer to/from device
	 */
	CS_MEM_TYPE_AFDM = 0,

	/**
	 * Device-only memory (not host visible)
	 * Reserved for future use
	 */
	CS_MEM_TYPE_DEVICE = 1,
} csMemType;

/** Memory allocation descriptor */
typedef struct {
	csMemType type;         /**< Type of memory to allocate */
	size_t size;            /**< Size in bytes */
	uint32_t alignment;     /**< Required alignment (0 for default) */
	uint32_t flags;         /**< Reserved for future use, set to 0 */
} csMemDesc;

/** @} */

/**
 * @defgroup cs_compute Compute Request Types
 * @{
 */

/** Compute request descriptor */
typedef struct {
	csCSFId csf_id;             /**< CSF to execute */
	const void *input_buffer;   /**< Input data buffer (host address) */
	size_t input_size;          /**< Size of input data in bytes */
	void *output_buffer;        /**< Output data buffer (host address) */
	size_t output_size;         /**< Size of output buffer in bytes */
	const void *args;           /**< Pointer to CSF-specific arguments */
	size_t args_size;           /**< Size of arguments in bytes */
	uint32_t nvm_nsid;          /**< NVM namespace for device-side staging.
	                              *   0 = use input_buffer (host path).
	                              *   >0 = backend reads from NVM, ignores
	                              *   input_buffer. */
	uint64_t nvm_slba;          /**< Starting LBA in NVM namespace */
	uint32_t slm_nsid;          /**< SLM namespace for MRS staging target */
	uint64_t slm_byte_off;      /**< Byte offset in SLM for staged data */
	uint64_t threshold;         /**< FILTER_GT comparison value; 0 = no threshold */
	uint64_t batch;             /**< Number of vector pairs for batch workloads
	                              *   (DOT_PRODUCT/L2/COSINE). 0 or 1 = single. */
} csComputeDesc;

/** Compute completion callback function type */
typedef void (*csComputeCallback)(csStatus status, void *user_data);

/** @} */

/**
 * @defgroup cs_device Device Discovery and Management
 * @{
 */

/** Device information structure */
typedef struct {
	char uri[256];              /**< Device URI/path */
	char vendor[64];            /**< Vendor name */
	char model[128];            /**< Model name */
	uint32_t max_programs;      /**< Maximum activated programs */
	uint32_t max_memory_mb;     /**< Maximum program memory in MiB */
	uint32_t flags;             /**< Device capability flags */
} csDeviceInfo;

/**
 * Query list of available CSx devices
 *
 * @param[out] devices      Array to store device info, or NULL to query count
 * @param[in,out] count     On input, size of devices array; on output, number of devices
 *
 * @return CS_STATUS_SUCCESS on success, or error code
 */
csStatus csQueryCSxList(csDeviceInfo *devices, uint32_t *count);

/**
 * Open a CSx device by URI
 *
 * @param[in] uri           Device URI (e.g., "/dev/nvme0n1")
 * @param[out] handle       Returned device handle
 *
 * @return CS_STATUS_SUCCESS on success, or error code
 */
csStatus csOpenCSx(const char *uri, csCSxHandle *handle);

/**
 * Close a CSx device handle
 *
 * @param[in] handle        Device handle to close
 *
 * @return CS_STATUS_SUCCESS on success, or error code
 */
csStatus csCloseCSx(csCSxHandle handle);

/**
 * Get information about an open CSx device
 *
 * @param[in] handle        Device handle
 * @param[out] info         Device information structure
 *
 * @return CS_STATUS_SUCCESS on success, or error code
 */
csStatus csGetCSxInfo(csCSxHandle handle, csDeviceInfo *info);

/** @} */

/**
 * @defgroup cs_memory_mgmt Memory Management
 * @{
 */

/**
 * Allocate memory for computational storage operations
 *
 * @param[in] csx_handle    CSx device handle
 * @param[in] desc          Memory allocation descriptor
 * @param[out] mem_handle   Returned memory handle
 * @param[out] ptr          Returned host pointer (for AFDM memory)
 *
 * @return CS_STATUS_SUCCESS on success, or error code
 */
csStatus csAllocMem(csCSxHandle csx_handle, const csMemDesc *desc,
		    csMemHandle *mem_handle, void **ptr);

/**
 * Free previously allocated memory
 *
 * @param[in] csx_handle    CSx device handle
 * @param[in] mem_handle    Memory handle to free
 *
 * @return CS_STATUS_SUCCESS on success, or error code
 */
csStatus csFreeMem(csCSxHandle csx_handle, csMemHandle mem_handle);

/**
 * Initialize memory contents to a fill value
 *
 * @param[in] csx_handle    CSx device handle
 * @param[in] mem_handle    Memory handle to initialize
 * @param[in] fill_value    Byte value to fill with
 * @param[in] size          Number of bytes to fill (0 = full allocation)
 *
 * @return CS_STATUS_SUCCESS on success, or error code
 */
csStatus csInitMem(csCSxHandle csx_handle, csMemHandle mem_handle,
		   uint8_t fill_value, size_t size);

/** Memory allocation statistics for a device */
typedef struct {
	uint32_t allocation_count;     /**< Current allocation count */
	uint32_t max_allocations;      /**< Maximum supported allocations */
	size_t total_allocated_bytes;  /**< Total allocated bytes */
} csMemInfo;

/**
 * Query memory allocation statistics for a device
 *
 * @param[in] csx_handle    CSx device handle
 * @param[out] info         Memory information structure
 *
 * @return CS_STATUS_SUCCESS on success, or error code
 */
csStatus csGetMemInfo(csCSxHandle csx_handle, csMemInfo *info);

/** @} */

/**
 * @defgroup cs_data_movement Data Movement
 * @{
 */

/** Copy direction */
typedef enum {
	CS_COPY_HOST_TO_DEVICE = 0,
	CS_COPY_DEVICE_TO_HOST = 1,
	CS_COPY_DEVICE_TO_DEVICE = 2,
	CS_COPY_HOST_TO_HOST = 3,
} csCopyDirection;

/** Storage operation */
typedef enum {
	CS_STORAGE_READ = 0,
	CS_STORAGE_WRITE = 1,
} csStorageOp;

/**
 * Queue a memory copy request between host and device memory
 *
 * @param[in] csx_handle    CSx device handle
 * @param[in] callback      Callback function (NULL for none)
 * @param[in] event         Event for completion notification (NULL for sync)
 * @param[in] src           Source pointer or memory handle (cast to void *)
 * @param[in] dst           Destination pointer or memory handle (cast to void *)
 * @param[in] size          Number of bytes to copy
 * @param[in] dir           Copy direction
 *
 * @return CS_STATUS_SUCCESS for sync completion, CS_STATUS_PENDING for async,
 *         or error code
 */
csStatus csQueueCopyMemRequest(csCSxHandle csx_handle, csComputeCallback callback,
			       csEventHandle event, void *src, void *dst,
			       size_t size, csCopyDirection dir);

/**
 * Queue a storage read/write request through the device
 *
 * @param[in] csx_handle    CSx device handle
 * @param[in] callback      Callback function (NULL for none)
 * @param[in] event         Event for completion notification (NULL for sync)
 * @param[in] mem           Memory handle for data buffer
 * @param[in] lba           Starting LBA
 * @param[in] blocks        Number of blocks
 * @param[in] op            Storage operation
 *
 * @return CS_STATUS_SUCCESS for sync completion, CS_STATUS_PENDING for async,
 *         or error code
 */
csStatus csQueueStorageRequest(csCSxHandle csx_handle, csComputeCallback callback,
			       csEventHandle event, csMemHandle mem,
			       uint64_t lba, uint32_t blocks, csStorageOp op);

/** @} */

/**
 * @defgroup cs_programs Program Management
 * @{
 */

/**
 * CSF information structure
 */
typedef struct {
	char name[64];          /**< CSF name (if available) */
	uint32_t version;       /**< Version number (if available) */
	uint32_t flags;         /**< Capability flags */
} csCSFInfo;

/**
 * Get CSF identifier for a program
 *
 * @param[in] csx_handle    CSx device handle
 * @param[in] prog_index    Program index on the device
 * @param[out] csf_id       Returned CSF identifier
 *
 * @return CS_STATUS_SUCCESS on success, or error code
 */
csStatus csGetCSFId(csCSxHandle csx_handle, uint16_t prog_index, csCSFId *csf_id);

/**
 * Get CSF information by identifier
 *
 * @param[in] csx_handle    CSx device handle
 * @param[in] csf_id        CSF identifier
 * @param[out] info         CSF information structure
 *
 * @return CS_STATUS_SUCCESS on success, or error code
 */
csStatus csGetCSFInfo(csCSxHandle csx_handle, csCSFId csf_id, csCSFInfo *info);

/**
 * Get CSF identifier by name (spec-compliant lookup)
 *
 * @param[in] csx_handle    CSx device handle
 * @param[in] name          CSF name (e.g., "SUM64", "FILTER_GT")
 * @param[out] csf_id       Returned CSF identifier
 *
 * @return CS_STATUS_SUCCESS on success, CS_STATUS_ERROR_NOT_FOUND if name unknown
 */
csStatus csGetCSFIdByName(csCSxHandle csx_handle, const char *name, csCSFId *csf_id);

/**
 * Query list of available Computational Storage Functions on a device
 *
 * @param[in] csx_handle    CSx device handle
 * @param[out] csfs         Array to store CSF info, or NULL to query count
 * @param[in,out] count     On input, size of array; on output, number of CSFs
 *
 * @return CS_STATUS_SUCCESS on success, or error code
 */
csStatus csQueryCSFList(csCSxHandle csx_handle, csCSFInfo *csfs, uint32_t *count);

/**
 * Load a computational program onto the device
 *
 * @param[in] csx_handle    CSx device handle
 * @param[in] prog_index    Target program slot index
 * @param[in] program       Program binary data
 * @param[in] program_size  Size of program in bytes
 *
 * @return CS_STATUS_SUCCESS on success, or error code
 */
csStatus csLoadProgram(csCSxHandle csx_handle, uint16_t prog_index,
		       const void *program, size_t program_size);

/**
 * Activate a loaded program
 *
 * @param[in] csx_handle    CSx device handle
 * @param[in] prog_index    Program slot index to activate
 *
 * @return CS_STATUS_SUCCESS on success, or error code
 */
csStatus csActivateProgram(csCSxHandle csx_handle, uint16_t prog_index);

/**
 * Deactivate a program
 *
 * @param[in] csx_handle    CSx device handle
 * @param[in] prog_index    Program slot index to deactivate
 *
 * @return CS_STATUS_SUCCESS on success, or error code
 */
csStatus csDeactivateProgram(csCSxHandle csx_handle, uint16_t prog_index);

/** @} */

/**
 * @defgroup cs_batch Batch Operations
 * @{
 */

/** Batch request type */
typedef enum {
	CS_REQ_TYPE_COPY_MEM = 0,
	CS_REQ_TYPE_STORAGE = 1,
	CS_REQ_TYPE_COMPUTE = 2,
} csRequestType;

/** Batch request descriptor */
typedef struct {
	csRequestType type;
	union {
		struct {
			void *src;
			void *dst;
			size_t size;
			csCopyDirection dir;
		} copy;
		struct {
			csMemHandle mem;
			uint64_t lba;
			uint32_t blocks;
			csStorageOp op;
		} storage;
		struct {
			csComputeDesc desc;
		} compute;
	} u;
} csRequest;

/**
 * Queue a batch of requests
 *
 * @param[in] csx_handle    CSx device handle
 * @param[in] callback      Callback function (NULL for none)
 * @param[in] event         Event for completion notification (NULL for sync)
 * @param[in] batch         Array of requests
 * @param[in] count         Number of requests
 *
 * @return CS_STATUS_SUCCESS for sync completion, CS_STATUS_PENDING for async,
 *         or error code
 */
csStatus csQueueBatchRequest(csCSxHandle csx_handle, csComputeCallback callback,
			     csEventHandle event, const csRequest *batch,
			     uint32_t count);

/** Opaque handle to a batch request */
typedef uint64_t csBatchHandle;

/** Batch execution mode */
typedef enum {
	CS_BATCH_MODE_SERIALIZED = 0,
	CS_BATCH_MODE_PARALLEL   = 1,
	CS_BATCH_MODE_HYBRID     = 2,
} csBatchMode;

/**
 * Allocate a batch request container
 *
 * @param[in] csx_handle    CSx device handle
 * @param[in] mode          Batch execution mode (serialized/parallel/hybrid)
 * @param[in] max_entries   Capacity hint for entries
 * @param[out] batch        Returned batch handle
 *
 * @return CS_STATUS_SUCCESS on success, or error code
 */
csStatus csAllocBatchRequest(csCSxHandle csx_handle, csBatchMode mode,
			     uint32_t max_entries, csBatchHandle *batch);

/**
 * Append one request to a batch
 *
 * @param[in] batch         Batch handle from csAllocBatchRequest()
 * @param[in] entry          Request descriptor to append
 *
 * @return CS_STATUS_SUCCESS on success, CS_STATUS_ERROR_LIMIT when the
 *         batch is full, or error code
 */
csStatus csAddBatchEntry(csBatchHandle batch, const csRequest *entry);

/**
 * Replace an existing batch entry in place
 *
 * @param[in] batch         Batch handle from csAllocBatchRequest()
 * @param[in] entry_index   Zero-based index of the entry to replace
 * @param[in] new_entry     Replacement request descriptor
 *
 * @return CS_STATUS_SUCCESS on success, CS_STATUS_ERROR_INVALID_PARAM when
 *         the index is out of range, or error code
 */
csStatus csHelperReconfigureBatchEntry(csBatchHandle batch, uint32_t entry_index,
				       const csRequest *new_entry);

/**
 * Change the capacity of a batch request
 *
 * @param[in] batch         Batch handle from csAllocBatchRequest()
 * @param[in] new_size      New capacity in entries
 *
 * @return CS_STATUS_SUCCESS on success, or error code
 */
csStatus csHelperResizeBatchRequest(csBatchHandle batch, uint32_t new_size);

/**
 * Release a batch request container
 *
 * @param[in] csx_handle    CSx device handle
 * @param[in] batch         Batch handle to free
 *
 * @return CS_STATUS_SUCCESS on success, or error code
 */
csStatus csFreeBatchRequest(csCSxHandle csx_handle, csBatchHandle batch);

/**
 * Submit a pre-allocated batch for execution
 *
 * Entries accumulated via csAddBatchEntry() are dispatched to the device.
 * In SERIALIZED mode each entry completes before the next begins; in
 * PARALLEL mode entries are submitted without blocking and completed
 * in arrival order.  When the backend lacks a native batch primitive the
 * library falls back to sequential submission.
 *
 * @param[in] csx_handle    CSx device handle
 * @param[in] batch         Batch handle from csAllocBatchRequest()
 * @param[in] callback      Callback function (NULL for none)
 * @param[in] event         Event for completion (CS_INVALID_HANDLE for sync)
 *
 * @return CS_STATUS_SUCCESS for sync completion, CS_STATUS_PENDING for async,
 *         or error code
 */
csStatus csSubmitBatchRequest(csCSxHandle csx_handle, csBatchHandle batch,
			      csComputeCallback callback, csEventHandle event);

/** @} */

/**
 * @defgroup cs_compute_ops Compute Operations
 * @{
 */

/**
 * Queue a compute request for execution
 *
 * @param[in] csx_handle    CSx device handle
 * @param[in] desc          Compute request descriptor
 * @param[in] event         Event for completion notification (NULL for sync)
 * @param[in] callback      Callback function for completion (NULL for none)
 * @param[in] user_data     User data passed to callback
 *
 * @return CS_STATUS_SUCCESS for sync completion, CS_STATUS_PENDING for async,
 *         or error code
 *
 * @note If both event and callback are NULL, the request executes synchronously
 */
csStatus csQueueComputeRequest(csCSxHandle csx_handle, const csComputeDesc *desc,
			       csEventHandle event, csComputeCallback callback,
			       void *user_data);

/**
 * Helper function to set compute arguments in a buffer
 *
 * @param[out] args_buffer  Buffer to write arguments into
 * @param[in] args_size     Size of args buffer
 * @param[in] arg_index     Index of argument to set
 * @param[in] arg_value     Pointer to argument value
 * @param[in] value_size    Size of argument value
 *
 * @return CS_STATUS_SUCCESS on success, or error code
 */
csStatus csHelperSetComputeArg(void *args_buffer, size_t args_size,
			       uint32_t arg_index, const void *arg_value,
			       size_t value_size);

/**
 * Abort an in-flight request associated with an event
 *
 * @param[in] csx_handle    CSx device handle
 * @param[in] event         Event handle associated with the request to abort
 *
 * @return CS_STATUS_SUCCESS on success, CS_STATUS_ERROR_NOT_SUPPORTED if
 *         async submission is not available, or error code
 */
csStatus csAbortRequest(csCSxHandle csx_handle, csEventHandle event);

/** @} */

/**
 * @defgroup cs_events Event Management
 * @{
 */

/** Event status values */
typedef enum {
	CS_EVENT_STATUS_PENDING = 0,    /**< Event not yet signaled */
	CS_EVENT_STATUS_COMPLETE = 1,   /**< Event signaled with success */
	CS_EVENT_STATUS_ERROR = 2,      /**< Event signaled with error */
} csEventStatus;

/**
 * Create an event for completion notification
 *
 * @param[in] csx_handle    CSx device handle
 * @param[out] event        Returned event handle
 *
 * @return CS_STATUS_SUCCESS on success, or error code
 */
csStatus csCreateEvent(csCSxHandle csx_handle, csEventHandle *event);

/**
 * Destroy an event
 *
 * @param[in] csx_handle    CSx device handle
 * @param[in] event         Event handle to destroy
 *
 * @return CS_STATUS_SUCCESS on success, or error code
 */
csStatus csDestroyEvent(csCSxHandle csx_handle, csEventHandle event);

/** Spec-compliant alias for csDestroyEvent */
#define csDeleteEvent csDestroyEvent

/**
 * Poll an event for completion status
 *
 * @param[in] csx_handle    CSx device handle
 * @param[in] event         Event handle to poll
 * @param[out] status       Event status
 * @param[out] result       Completion result (CS_STATUS_SUCCESS or error)
 *
 * @return CS_STATUS_SUCCESS on success, or error code
 */
csStatus csPollEvent(csCSxHandle csx_handle, csEventHandle event,
		     csEventStatus *status, csStatus *result);

/**
 * Wait for an event to complete with timeout
 *
 * @param[in] csx_handle    CSx device handle
 * @param[in] event         Event handle to wait on
 * @param[in] timeout_ms    Timeout in milliseconds (0 for infinite)
 * @param[out] result       Completion result
 *
 * @return CS_STATUS_SUCCESS on success, CS_STATUS_ERROR_TIMEOUT on timeout
 */
csStatus csWaitEvent(csCSxHandle csx_handle, csEventHandle event,
		     uint32_t timeout_ms, csStatus *result);

/** @} */

/**
 * @defgroup cs_library Library Initialization
 * @{
 */

/**
 * Initialize the CS API library
 *
 * @return CS_STATUS_SUCCESS on success, or error code
 *
 * @note This function is called automatically on first use, but can be
 *       called explicitly to control initialization timing
 */
csStatus csInit(void);

/**
 * Shutdown the CS API library
 *
 * @return CS_STATUS_SUCCESS on success, or error code
 *
 * @note All handles should be closed before calling this function. Any
 *       device handle still open is force-closed (with a warning): the
 *       backend's dev_close is invoked and the handle slot is released
 *       without running the normal per-device close path, so data still in
 *       flight on that handle may be lost.
 */
csStatus csShutdown(void);

/**
 * Get library version string
 *
 * @return Version string (e.g., "1.0.0")
 */
const char *csGetVersion(void);

/** @} */


/**
 * @defgroup cs_device_mgmt Device Management
 * @{
 */

/** Static device properties (SNIA CS API §5.1) */
typedef struct {
	uint32_t num_cses;          /**< Number of Computational Storage Engines */
	uint32_t num_csees;         /**< Number of Computational Storage Engines-Enhanced */
	size_t   fdm_total_bytes;  /**< Total Fixed Device Memory bytes */
	size_t   afdm_total_bytes; /**< Total Application-Facing Device Memory bytes */
	uint32_t max_concurrent_progs; /**< Max concurrently active programs */
	uint32_t csf_count;         /**< Number of supported CSFs */
} csDeviceProperties;

/** Cumulative device activity counters */
typedef struct {
	uint64_t total_executes;         /**< Executions since reset */
	uint64_t total_bytes_processed;  /**< Bytes processed since reset */
	uint64_t total_errors;           /**< Failed executions since reset */
	uint64_t avg_latency_ns;         /**< Average execution latency (ns) */
} csDeviceStatistics;

/**
 * Query static device properties
 *
 * @param[in]  csx_handle  Device handle
 * @param[out] props       Returned device properties
 *
 * @return CS_STATUS_SUCCESS on success, or error code
 */
csStatus csQueryDeviceProperties(csCSxHandle csx_handle, csDeviceProperties *props);

/**
 * Query cumulative device statistics
 *
 * @param[in]  csx_handle  Device handle
 * @param[out] stats      Returned device statistics
 *
 * @return CS_STATUS_SUCCESS on success, or error code
 */
csStatus csQueryDeviceStatistics(csCSxHandle csx_handle, csDeviceStatistics *stats);

/**
 * Configure a device parameter
 *
 * @param[in] csx_handle  Device handle
 * @param[in] config_id    Configuration parameter ID
 * @param[in] data        Configuration payload
 * @param[in] size        Payload size in bytes
 *
 * @return CS_STATUS_SUCCESS on success, CS_STATUS_ERROR_NOT_SUPPORTED if the
 *         backend does not implement device configuration, or error code
 */
csStatus csConfig(csCSxHandle csx_handle, uint32_t config_id, const void *data, size_t size);

/**
 * Reset a device or its statistics
 *
 * @param[in] csx_handle  Device handle
 * @param[in] reset_type  Reset type (device, statistics, or both)
 *
 * @return CS_STATUS_SUCCESS on success, CS_STATUS_ERROR_NOT_SUPPORTED if the
 *         backend does not implement reset, or error code
 */
csStatus csReset(csCSxHandle csx_handle, uint32_t reset_type);

/**
 * Download a Computational Storage Function image
 *
 * @param[in]  csx_handle  Device handle
 * @param[in]  csf_binary  CSF image bytes
 * @param[in]  size        Image size in bytes
 * @param[out] csf_id      Returned CSF identifier
 *
 * @return CS_STATUS_SUCCESS on success, CS_STATUS_ERROR_NOT_SUPPORTED if the
 *         backend does not support downloadable CSFs, or error code
 */
csStatus csCSFDownload(csCSxHandle csx_handle, const void *csf_binary,
		       size_t size, csCSFId *csf_id);

/**
 * Download a Computational Storage Engine-Enhanced image
 *
 * @param[in]  csx_handle  Device handle
 * @param[in]  image       CSEE image bytes
 * @param[in]  size        Image size in bytes
 * @param[out] csee_id     Returned CSEE identifier
 *
 * @return CS_STATUS_SUCCESS on success, CS_STATUS_ERROR_NOT_SUPPORTED if the
 *         backend does not support downloadable CSEEs, or error code
 */
csStatus csCSEEDownload(csCSxHandle csx_handle, const void *image,
			size_t size, uint32_t *csee_id);

/**
 * @brief Group CSFs for optimized scheduling on a shared CSE
 *
 * Per SNIA CS API v1.0 §5.2.2: when multiple CSFs are queued on a single
 * CSE, grouping provides scheduling hints to minimize idle time between
 * executions.  The returned group ID can be passed to csQueueComputeRequest
 * via the args field and dissolved with csUngroupComputeFromGroupId().
 *
 * @param[in]  csx_handle  Device handle
 * @param[in]  csf_ids     Array of CSF IDs to group
 * @param[in]  count       Number of CSF IDs
 * @param[out] group_id    Returned group identifier
 *
 * @return CS_STATUS_SUCCESS on success, or error code
 */
csStatus csGroupComputeByIds(csCSxHandle csx_handle, const csCSFId *csf_ids,
			     uint32_t count, uint32_t *group_id);

/**
 * @brief Dissolve a previously created compute group
 *
 * @param[in]  csx_handle  Device handle
 * @param[in]  group_id    Group identifier from csGroupComputeByIds()
 *
 * @return CS_STATUS_SUCCESS on success, or error code
 */
csStatus csUngroupComputeFromGroupId(csCSxHandle csx_handle, uint32_t group_id);

#define CS_FEATURE_ASYNC_COMPUTE  0x01u
#define CS_FEATURE_BATCH          0x02u
#define CS_FEATURE_EVENTS          0x04u
#define CS_FEATURE_EBPF           0x08u

/** Library capability summary */
typedef struct {
	uint32_t api_version;   /**< API version */
	uint32_t num_backends;  /**< Number of registered backends */
	uint32_t features;     /**< Bitwise OR of CS_FEATURE_* flags */
} csLibrarySupport;

/**
 * Query library capability summary
 *
 * @param[out] support  Returned library support information
 *
 * @return CS_STATUS_SUCCESS on success, or error code
 */
csStatus csQueryLibrarySupport(csLibrarySupport *support);

/**
 * Register a backend plugin with the library
 *
 * @param[in] ops  Backend operations vtable (see cs_backend.h)
 *
 * @return CS_STATUS_SUCCESS on success, CS_STATUS_ERROR_ALREADY_EXISTS if a
 *         backend with the same name is already registered, or error code
 *
 * @deprecated Superseded by cs_backend_register() (cs_backend.h); the two APIs
 *             share one registry. This entry point will be removed in the next
 *             major release.
 */
struct cs_backend_ops;
csStatus csRegisterPlugin(const struct cs_backend_ops *ops);

/**
 * Deregister a backend plugin by name
 *
 * @param[in] name  Backend name to remove
 *
 * @return CS_STATUS_SUCCESS on success, CS_STATUS_ERROR_NOT_FOUND if no
 *         backend with that name is registered, or error code
 *
 * @deprecated Superseded by cs_backend_unregister(); will be removed in the
 *             next major release.
 */
csStatus csDeregisterPlugin(const char *name);

typedef void (*csNotifyCallback)(uint32_t event_type, void *user_data);

/**
 * Register a callback for device notifications
 *
 * @param[in] csx_handle   Device handle
 * @param[in] event_type   Notification event type to subscribe to
 * @param[in] callback     Callback invoked on the event (NULL to unsubscribe)
 * @param[in] user_data    Opaque context passed to the callback
 *
 * @return CS_STATUS_SUCCESS on success, or error code
 */
csStatus csRegisterNotify(csCSxHandle csx_handle, uint32_t event_type,
			  csNotifyCallback callback, void *user_data);

/**
 * Remove a previously registered notification callback
 *
 * @param[in] csx_handle   Device handle
 * @param[in] event_type   Notification event type to unsubscribe from
 *
 * @return CS_STATUS_SUCCESS on success, CS_STATUS_ERROR_NOT_FOUND if no
 *         callback is registered for the event type, or error code
 */
csStatus csDeregisterNotify(csCSxHandle csx_handle, uint32_t event_type);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* CS_API_H */
