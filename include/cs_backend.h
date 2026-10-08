/**
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @file cs_backend.h
 * @brief CS API Backend Plugin Interface
 *
 * This header defines the interface for backend plugins that provide
 * the actual implementation for computational storage operations.
 */

#ifndef CS_BACKEND_H
#define CS_BACKEND_H

#include "cs_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Forward declaration of backend context
 */
struct cs_backend_ctx;

/**
 * Backend operations vtable
 *
 * Each backend must implement these operations to provide
 * computational storage functionality.
 */
typedef struct cs_backend_ops {
	/** Backend name (e.g., "xnvme") */
	const char *name;

	/**
	 * Initialize the backend
	 *
	 * @return CS_STATUS_SUCCESS on success, or error code
	 */
	csStatus (*init)(void);

	/**
	 * Shutdown the backend
	 *
	 * @return CS_STATUS_SUCCESS on success, or error code
	 */
	csStatus (*shutdown)(void);

	/**
	 * Query available devices
	 *
	 * @param[out] devices      Array to store device info, or NULL
	 * @param[in,out] count     On input, size of array; on output, device count
	 *
	 * @return CS_STATUS_SUCCESS on success, or error code
	 */
	csStatus (*dev_query)(csDeviceInfo *devices, uint32_t *count);

	/**
	 * Open a device
	 *
	 * @param[in] uri           Device URI
	 * @param[out] backend_ctx  Backend-specific context
	 *
	 * @return CS_STATUS_SUCCESS on success, or error code
	 */
	csStatus (*dev_open)(const char *uri, struct cs_backend_ctx **backend_ctx);

	/**
	 * Close a device
	 *
	 * @param[in] backend_ctx   Backend context from dev_open
	 *
	 * @return CS_STATUS_SUCCESS on success, or error code
	 */
	csStatus (*dev_close)(struct cs_backend_ctx *backend_ctx);

	/**
	 * Get device information
	 *
	 * @param[in] backend_ctx   Backend context
	 * @param[out] info         Device information
	 *
	 * @return CS_STATUS_SUCCESS on success, or error code
	 */
	csStatus (*dev_get_info)(struct cs_backend_ctx *backend_ctx, csDeviceInfo *info);

	/**
	 * Allocate DMA-capable memory
	 *
	 * @param[in] backend_ctx   Backend context
	 * @param[in] size          Size in bytes
	 * @param[in] alignment     Required alignment
	 * @param[out] ptr          Returned host pointer
	 *
	 * @return CS_STATUS_SUCCESS on success, or error code
	 */
	csStatus (*mem_alloc)(struct cs_backend_ctx *backend_ctx, size_t size,
			      uint32_t alignment, void **ptr);

	/**
	 * Free DMA-capable memory
	 *
	 * @param[in] backend_ctx   Backend context
	 * @param[in] ptr           Pointer from mem_alloc
	 *
	 * @return CS_STATUS_SUCCESS on success, or error code
	 */
	csStatus (*mem_free)(struct cs_backend_ctx *backend_ctx, void *ptr);

	/**
	 * Load a program onto the device
	 *
	 * @param[in] backend_ctx   Backend context
	 * @param[in] prog_index    Program slot index
	 * @param[in] program       Program binary data
	 * @param[in] program_size  Size of program
	 *
	 * @return CS_STATUS_SUCCESS on success, or error code
	 */
	csStatus (*prog_load)(struct cs_backend_ctx *backend_ctx, uint16_t prog_index,
			      const void *program, size_t program_size);

	/**
	 * Activate a loaded program
	 *
	 * @param[in] backend_ctx   Backend context
	 * @param[in] prog_index    Program slot index
	 *
	 * @return CS_STATUS_SUCCESS on success, or error code
	 */
	csStatus (*prog_activate)(struct cs_backend_ctx *backend_ctx, uint16_t prog_index);

	/**
	 * Deactivate a program
	 *
	 * @param[in] backend_ctx   Backend context
	 * @param[in] prog_index    Program slot index
	 *
	 * @return CS_STATUS_SUCCESS on success, or error code
	 */
	csStatus (*prog_deactivate)(struct cs_backend_ctx *backend_ctx, uint16_t prog_index);

	/**
	 * Get program name by index
	 *
	 * @param[in] backend_ctx   Backend context
	 * @param[in] prog_index    Program slot index
	 * @param[out] name         Buffer to store name
	 * @param[in] name_size     Size of name buffer
	 *
	 * @return CS_STATUS_SUCCESS on success, or error code
	 */
	csStatus (*prog_get_name)(struct cs_backend_ctx *backend_ctx, uint16_t prog_index,
				  char *name, size_t name_size);

	/**
	 * Query list of available CSFs on the device
	 *
	 * @param[in] backend_ctx   Backend context
	 * @param[out] csfs         Array to store CSF info, or NULL for count
	 * @param[in,out] count     On input, array size; on output, CSF count
	 *
	 * @return CS_STATUS_SUCCESS on success, or error code
	 */
	csStatus (*query_csf_list)(struct cs_backend_ctx *backend_ctx,
				   csCSFInfo *csfs, uint32_t *count);

	/**
	 * Initialize memory contents to a fill value
	 *
	 * @param[in] backend_ctx   Backend context
	 * @param[in] ptr           Host pointer from mem_alloc
	 * @param[in] fill_value    Byte value to fill
	 * @param[in] size          Bytes to fill
	 *
	 * @return CS_STATUS_SUCCESS on success, or error code
	 */
	csStatus (*mem_init)(struct cs_backend_ctx *backend_ctx, void *ptr,
			     uint8_t fill_value, size_t size);

	/**
	 * Execute a compute operation
	 *
	 * @param[in] backend_ctx   Backend context
	 * @param[in] prog_index    Program index to execute
	 * @param[in] input         Input buffer
	 * @param[in] input_size    Input size
	 * @param[out] output       Output buffer
	 * @param[in] output_size   Output buffer size
	 * @param[in] args          Program arguments
	 * @param[in] args_size     Arguments size
	 * @param[out] completion   Completion status pointer (for async)
	 *
	 * @return CS_STATUS_SUCCESS or CS_STATUS_PENDING on success, or error code
	 */
	csStatus (*compute_execute)(struct cs_backend_ctx *backend_ctx, uint16_t prog_index,
				    const void *input, size_t input_size,
				    void *output, size_t output_size,
				    const void *args, size_t args_size,
				    volatile int *completion);

	/**
	 * Execute a compute operation on data resident in NVM.
	 *
	 * The backend stages the NVM region into SLM via a device-internal
	 * copy, creates an MRS pointing to the SLM region, and executes
	 * the program via a descriptor-only MRS command.  No host-to-device
	 * data transfer occurs.
	 *
	 * May be NULL if the backend does not support NVM-origin compute.
	 *
	 * @param[in] backend_ctx   Backend context
	 * @param[in] prog_index    Program index to execute
	 * @param[in] nvm_nsid      Source NVM namespace ID
	 * @param[in] nvm_slba      Starting LBA in NVM namespace
	 * @param[in] slm_nsid      Destination SLM namespace ID
	 * @param[in] slm_byte_off  Byte offset in SLM for staged data
	 * @param[in] data_size     Number of bytes to read from NVM
	 * @param[out] output       Output buffer (host address)
	 * @param[in] output_size   Output buffer size
	 * @param[out] completion   Completion status pointer (for async)
	 *
	 * @return CS_STATUS_SUCCESS or CS_STATUS_PENDING on success, or error code
	 */
	csStatus (*compute_execute_nvm)(struct cs_backend_ctx *backend_ctx,
					uint16_t prog_index,
					uint32_t nvm_nsid, uint64_t nvm_slba,
					uint32_t slm_nsid, uint64_t slm_byte_off,
					size_t data_size,
					void *output, size_t output_size,
					uint64_t threshold, uint64_t batch,
					volatile int *completion);

	/**
	 * Submit a compute operation asynchronously (non-blocking)
	 *
	 * Submits the NVMe command to the submission queue and returns immediately.
	 * Completion is signaled via the completion flag or reaped via compute_reap.
	 *
	 * @param[in] backend_ctx   Backend context
	 * @param[in] prog_index    Program index to execute
	 * @param[in] input         Input buffer
	 * @param[in] input_size    Input size
	 * @param[out] output       Output buffer
	 * @param[in] output_size   Output buffer size
	 * @param[in] args          Program arguments
	 * @param[in] args_size     Arguments size
	 * @param[out] completion   Completion flag pointer (set by backend on completion)
	 *
	 * @return CS_STATUS_PENDING on successful submission, or error code
	 */
	csStatus (*compute_submit)(struct cs_backend_ctx *backend_ctx, uint16_t prog_index,
				   const void *input, size_t input_size,
				   void *output, size_t output_size,
				   const void *args, size_t args_size,
				   volatile int *completion);

	/**
	 * Reap completion of a previously submitted async compute operation
	 *
	 * @param[in] backend_ctx   Backend context
	 * @param[in] completion    Completion flag pointer from compute_submit
	 *
	 * @return CS_STATUS_PENDING if not yet complete,
	 *         CS_STATUS_SUCCESS if completed successfully,
	 *         or error code on failure
	 */
	csStatus (*compute_reap)(struct cs_backend_ctx *backend_ctx,
				 volatile int *completion);

	/**
	 * Abort an in-flight request
	 *
	 * @param[in] backend_ctx   Backend context
	 * @param[in] completion    Completion flag pointer associated with the request
	 *
	 * @return CS_STATUS_SUCCESS on success, CS_STATUS_ERROR_NOT_SUPPORTED if
	 *         not implemented, or error code
	 */
	csStatus (*abort_request)(struct cs_backend_ctx *backend_ctx,
				  volatile int *completion);

	/**
	 * Execute a storage read/write operation
	 *
	 * @param[in] backend_ctx   Backend context
	 * @param[in] op            Storage operation
	 * @param[in] lba           Starting LBA
	 * @param[in] blocks        Number of blocks
	 * @param[in,out] buffer    Data buffer
	 * @param[in] buffer_size   Data buffer size in bytes
	 * @param[out] completion   Completion status pointer (for async)
	 *
	 * @return CS_STATUS_SUCCESS or CS_STATUS_PENDING on success, or error code
	 */
	csStatus (*storage_io)(struct cs_backend_ctx *backend_ctx, csStorageOp op,
			       uint64_t lba, uint32_t blocks,
			       void *buffer, size_t buffer_size,
			       volatile int *completion);

	/**
	 * Poll for completion of async operations
	 *
	 * @param[in] backend_ctx   Backend context
	 * @param[in] max           Maximum completions to process (0 = all)
	 *
	 * @return Number of completions processed, or negative error code
	 */
	int (*poll)(struct cs_backend_ctx *backend_ctx, uint32_t max);

	csStatus (*batch_alloc)(struct cs_backend_ctx *backend_ctx, csBatchMode mode,
				uint32_t max_entries, csBatchHandle *batch);
	csStatus (*batch_add_entry)(csBatchHandle batch, const csRequest *entry);
	csStatus (*batch_submit)(struct cs_backend_ctx *backend_ctx, csBatchHandle batch,
				 csComputeCallback callback, csEventHandle event);
	csStatus (*batch_free)(struct cs_backend_ctx *backend_ctx, csBatchHandle batch);

	csStatus (*query_device_properties)(struct cs_backend_ctx *backend_ctx,
					    csDeviceProperties *props);
	csStatus (*query_device_statistics)(struct cs_backend_ctx *backend_ctx,
					    csDeviceStatistics *stats);
	csStatus (*device_config)(struct cs_backend_ctx *backend_ctx, uint32_t config_id,
				  const void *data, size_t size);
	csStatus (*device_reset)(struct cs_backend_ctx *backend_ctx, uint32_t reset_type);

} cs_backend_ops_t;

/**
 * Register a backend with the library
 *
 * @param[in] ops       Backend operations vtable
 *
 * @return CS_STATUS_SUCCESS on success, or error code
 */
csStatus cs_backend_register(const cs_backend_ops_t *ops);

/**
 * Get a backend by name
 *
 * @param[in] name      Backend name (NULL for default)
 *
 * @return Backend operations, or NULL if not found
 */
const cs_backend_ops_t *cs_backend_get(const char *name);

/**
 * Get the default backend
 *
 * @return Default backend operations, or NULL if none registered
 */
const cs_backend_ops_t *cs_backend_get_default(void);

/**
 * List all registered backends
 *
 * @param[out] names    Array to store backend names
 * @param[in,out] count On input, array size; on output, backend count
 *
 * @return CS_STATUS_SUCCESS on success, or error code
 */
csStatus cs_backend_list(const char **names, uint32_t *count);

#ifdef __cplusplus
}
#endif

#endif /* CS_BACKEND_H */
