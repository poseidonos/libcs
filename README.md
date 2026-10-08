# libcs - SNIA Computational Storage API Host Library

A host library implementing the SNIA Computational Storage API, wrapping the xNVMe library to provide computational storage functionality via NVMe devices.

## Table of Contents

- [Overview](#overview)
- [Architecture](#architecture)
- [Building](#building)
- [Quick Start](#quick-start)
- [API Reference](#api-reference)
- [Examples](#examples)
- [Design Details](#design-details)
- [Testing](#testing)
- [Troubleshooting](#troubleshooting)
- [License](#license)

## Overview

libcs provides a standardized C API for computational storage operations, enabling applications like DuckDB to offload compute tasks to NVMe devices that support the Computational Programs (CP) command set.

### Key Features

- **SNIA CS API Compliant**: Follows the SNIA Computational Storage API specification
- **Pluggable Backends**: Modular architecture supporting multiple storage backends
- **Async & Sync Operations**: Event-based async API with synchronous fallback
- **Thread-Safe**: Per-device locking with atomic event status updates
- **DMA Memory Management**: Automatic alignment and DMA-capable buffer allocation

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    Application Layer                         │
│  (DuckDB, Custom Apps)                                       │
└─────────────────────────────────────────────────────────────┘
                              │
                              │ csQueueComputeRequest()
                              │ csAllocMem() / csFreeMem()
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                      libcs.so                                │
│  ┌────────────────────────────────────────────────────────┐ │
│  │                    cs_api.c                             │ │
│  │  • Handle management (device, memory, event)           │ │
│  │  • Thread-safe context lookup                          │ │
│  │  • Backend dispatch                                    │ │
│  └────────────────────────────────────────────────────────┘ │
│  ┌────────────────────────────────────────────────────────┐ │
│  │                 Backend Interface                       │ │
│  │  cs_backend.h: vtable with dev/mem/compute operations  │ │
│  └────────────────────────────────────────────────────────┘ │
│  ┌────────────────────────────────────────────────────────┐ │
│  │              backend_xnvme.c (default)                  │ │
│  │  • Wraps xNVMe library                                 │ │
│  │  • Maps CS API → NVMe CP commands                      │ │
│  └────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
                              │
                              │ xnvme_cmd_cp_execute_buf()
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                       xNVMe Library                          │
│  • Device abstraction (SPDK, io_uring, etc.)                │
│  • NVMe command construction                                 │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                    NVMe CS Device                            │
│  • Computational Programs Command Set (NVMe TP4091)         │
│  • Program Load/Activate/Execute                            │
└─────────────────────────────────────────────────────────────┘
```

## Building

### Prerequisites

| Requirement | Version | Notes |
|-------------|---------|-------|
| Meson | >= 0.58 | Build system |
| Ninja | any | Build backend |
| xNVMe | >= 0.7.0 | CS device access |
| pthreads | - | Thread support |

### Standard Build

```bash
# Configure
meson setup build

# Compile
meson compile -C build

# Install (optional)
sudo meson install -C build
```

### Build with Local xNVMe

If xNVMe is not installed system-wide:

```bash
# Build xNVMe first (in parent directory)
cd ../xnvme
meson setup build -Dwith-spdk=disabled  # Minimal build
meson compile -C build

# Return and build libcs
cd ../libcs
meson setup build --wipe
meson compile -C build
```

### Build Options

```bash
# Debug build with symbols
meson setup build -Dbuildtype=debug

# Release build with optimizations
meson setup build -Dbuildtype=release
```

## Quick Start

### Minimal Example

```c
#include <cs_api.h>
#include <stdio.h>

int main() {
    csStatus status;

    // Initialize library
    status = csInit();
    if (status != CS_STATUS_SUCCESS) {
        fprintf(stderr, "Init failed: %d\n", status);
        return 1;
    }

    // Open device
    csCSxHandle device;
    status = csOpenCSx("/dev/nvme0n1", &device);
    if (status != CS_STATUS_SUCCESS) {
        fprintf(stderr, "Open failed: %d\n", status);
        csShutdown();
        return 1;
    }

    printf("Device opened successfully!\n");
    printf("Library version: %s\n", csGetVersion());

    // Get device info
    csDeviceInfo info;
    csGetCSxInfo(device, &info);
    printf("Device: %s\n", info.uri);
    printf("Max programs: %u\n", info.max_programs);

    // Cleanup
    csCloseCSx(device);
    csShutdown();

    return 0;
}
```

### Compile and Run

```bash
# Compile
gcc -o example example.c -lcs -lpthread

# Set device and run
export LIBCS_DEVICE_URI="/dev/nvme0n1"
./example
```

## API Reference

### Status Codes

```c
typedef enum {
    CS_STATUS_SUCCESS = 0,              // Operation completed successfully
    CS_STATUS_PENDING = 1,              // Operation is pending (async)

    // Error codes (negative values)
    CS_STATUS_ERROR_INVALID_HANDLE = -1,
    CS_STATUS_ERROR_INVALID_PARAM = -2,
    CS_STATUS_ERROR_NO_MEMORY = -3,
    CS_STATUS_ERROR_NOT_FOUND = -4,
    CS_STATUS_ERROR_BUSY = -5,
    CS_STATUS_ERROR_TIMEOUT = -6,
    CS_STATUS_ERROR_IO = -7,
    CS_STATUS_ERROR_NOT_SUPPORTED = -8,
    CS_STATUS_ERROR_PERMISSION = -9,
    CS_STATUS_ERROR_NO_DEVICE = -10,
    CS_STATUS_ERROR_BACKEND = -11,
    CS_STATUS_ERROR_NOT_INITIALIZED = -12,
    CS_STATUS_ERROR_ALREADY_EXISTS = -13,
    CS_STATUS_ERROR_LIMIT = -14,
} csStatus;
```

### Handle Types

```c
typedef uint64_t csCSxHandle;    // Device handle
typedef uint64_t csMemHandle;    // Memory allocation handle
typedef uint64_t csEventHandle;  // Completion event handle
typedef uint32_t csCSFId;        // Computational Storage Function ID

#define CS_INVALID_HANDLE ((uint64_t)0)
```

### Library Lifecycle

#### csInit
```c
csStatus csInit(void);
```
Initialize the CS API library. Called automatically on first API use, but can be called explicitly for controlled initialization.

**Returns**: `CS_STATUS_SUCCESS` or error code

#### csShutdown
```c
csStatus csShutdown(void);
```
Shutdown the library. All handles must be closed first.

**Returns**: `CS_STATUS_SUCCESS` or error code

#### csGetVersion
```c
const char *csGetVersion(void);
```
Get library version string.

**Returns**: Version string (e.g., "1.0.0")

### Device Management

#### csQueryCSxList
```c
csStatus csQueryCSxList(csDeviceInfo *devices, uint32_t *count);
```
Query available CS devices.

| Parameter | Description |
|-----------|-------------|
| `devices` | Array to store device info, or NULL to query count only |
| `count` | Input: array size; Output: number of devices |

**Returns**: `CS_STATUS_SUCCESS` or error code

#### csOpenCSx
```c
csStatus csOpenCSx(const char *uri, csCSxHandle *handle);
```
Open a CS device.

| Parameter | Description |
|-----------|-------------|
| `uri` | Device URI (e.g., "/dev/nvme0n1") |
| `handle` | Output: device handle |

**Returns**: `CS_STATUS_SUCCESS` or error code

#### csCloseCSx
```c
csStatus csCloseCSx(csCSxHandle handle);
```
Close a device handle.

| Parameter | Description |
|-----------|-------------|
| `handle` | Device handle to close |

**Returns**: `CS_STATUS_SUCCESS` or error code

#### csGetCSxInfo
```c
csStatus csGetCSxInfo(csCSxHandle handle, csDeviceInfo *info);
```
Get device information.

| Parameter | Description |
|-----------|-------------|
| `handle` | Device handle |
| `info` | Output: device information structure |

**Device Info Structure**:
```c
typedef struct {
    char uri[256];           // Device URI/path
    char vendor[64];         // Vendor name
    char model[128];         // Model name
    uint32_t max_programs;   // Maximum activated programs
    uint32_t max_memory_mb;  // Maximum program memory in MiB
    uint32_t flags;          // Capability flags
} csDeviceInfo;
```

### Memory Management

#### csAllocMem
```c
csStatus csAllocMem(csCSxHandle csx_handle, const csMemDesc *desc,
                    csMemHandle *mem_handle, void **ptr);
```
Allocate DMA-capable memory.

| Parameter | Description |
|-----------|-------------|
| `csx_handle` | Device handle |
| `desc` | Memory allocation descriptor |
| `mem_handle` | Output: memory handle |
| `ptr` | Output: host pointer |

**Memory Descriptor**:
```c
typedef struct {
    csMemType type;      // CS_MEM_TYPE_AFDM (host-visible)
    size_t size;         // Size in bytes
    uint32_t alignment;  // Required alignment (0 for default)
    uint32_t flags;      // Reserved, set to 0
} csMemDesc;
```

#### csFreeMem
```c
csStatus csFreeMem(csCSxHandle csx_handle, csMemHandle mem_handle);
```
Free allocated memory.

### Program Management

#### csGetCSFId
```c
csStatus csGetCSFId(csCSxHandle csx_handle, uint16_t prog_index, csCSFId *csf_id);
```
Get CSF identifier for a program slot.

#### csLoadProgram
```c
csStatus csLoadProgram(csCSxHandle csx_handle, uint16_t prog_index,
                       const void *program, size_t program_size);
```
Load a computational program onto the device.

#### csActivateProgram
```c
csStatus csActivateProgram(csCSxHandle csx_handle, uint16_t prog_index);
```
Activate a loaded program.

#### csDeactivateProgram
```c
csStatus csDeactivateProgram(csCSxHandle csx_handle, uint16_t prog_index);
```
Deactivate a program.

### Compute Operations

#### csQueueComputeRequest
```c
csStatus csQueueComputeRequest(csCSxHandle csx_handle, const csComputeDesc *desc,
                               csEventHandle event, csComputeCallback callback,
                               void *user_data);
```
Queue a compute request for execution.

| Parameter | Description |
|-----------|-------------|
| `csx_handle` | Device handle |
| `desc` | Compute request descriptor |
| `event` | Event for async notification (CS_INVALID_HANDLE for sync) |
| `callback` | Completion callback (NULL for none) |
| `user_data` | User data passed to callback |

**Compute Descriptor**:
```c
typedef struct {
    csCSFId csf_id;           // CSF to execute
    const void *input_buffer; // Input data (host address)
    size_t input_size;        // Input size in bytes
    void *output_buffer;      // Output buffer (host address)
    size_t output_size;       // Output buffer size
    const void *args;         // CSF-specific arguments
    size_t args_size;         // Arguments size
} csComputeDesc;
```

**Execution Modes**:
- **Synchronous**: `event = CS_INVALID_HANDLE`, `callback = NULL`
- **Async with event**: Provide event, poll/wait for completion
- **Async with callback**: Provide callback function

**Returns**:
- `CS_STATUS_SUCCESS` for sync completion
- `CS_STATUS_PENDING` for async (check event/await callback)
- Error code on failure

#### csHelperSetComputeArg
```c
csStatus csHelperSetComputeArg(void *args_buffer, size_t args_size,
                               uint32_t arg_index, const void *arg_value,
                               size_t value_size);
```
Helper to pack arguments into a buffer.

### Event Management

#### csCreateEvent
```c
csStatus csCreateEvent(csCSxHandle csx_handle, csEventHandle *event);
```
Create a completion event.

#### csDestroyEvent
```c
csStatus csDestroyEvent(csCSxHandle csx_handle, csEventHandle event);
```
Destroy an event.

#### csPollEvent
```c
csStatus csPollEvent(csCSxHandle csx_handle, csEventHandle event,
                     csEventStatus *status, csStatus *result);
```
Poll event for completion status.

**Event Status**:
```c
typedef enum {
    CS_EVENT_STATUS_PENDING = 0,   // Not yet signaled
    CS_EVENT_STATUS_COMPLETE = 1,  // Completed successfully
    CS_EVENT_STATUS_ERROR = 2,     // Completed with error
} csEventStatus;
```

#### csWaitEvent
```c
csStatus csWaitEvent(csCSxHandle csx_handle, csEventHandle event,
                     uint32_t timeout_ms, csStatus *result);
```
Wait for event completion with timeout.

| Parameter | Description |
|-----------|-------------|
| `timeout_ms` | Timeout in milliseconds (0 = infinite) |
| `result` | Output: completion result |

**Returns**: `CS_STATUS_SUCCESS` or `CS_STATUS_ERROR_TIMEOUT`

## Examples

### Synchronous Compute

```c
#include <cs_api.h>
#include <string.h>

int run_sync_compute(void) {
    csStatus status;
    csCSxHandle device;

    csInit();

    // Open device
    status = csOpenCSx("/dev/nvme0n1", &device);
    if (status != CS_STATUS_SUCCESS) return status;

    // Allocate input/output buffers
    csMemDesc desc = { .type = CS_MEM_TYPE_AFDM, .size = 4096 };
    csMemHandle input_mem, output_mem;
    void *input_ptr, *output_ptr;

    csAllocMem(device, &desc, &input_mem, &input_ptr);
    csAllocMem(device, &desc, &output_mem, &output_ptr);

    // Initialize input data
    memset(input_ptr, 0x55, 4096);

    // Get CSF ID and execute
    csCSFId csf_id;
    csGetCSFId(device, 0, &csf_id);

    csComputeDesc compute = {
        .csf_id = csf_id,
        .input_buffer = input_ptr,
        .input_size = 4096,
        .output_buffer = output_ptr,
        .output_size = 4096,
    };

    // Synchronous execution (blocking)
    status = csQueueComputeRequest(device, &compute,
                                   CS_INVALID_HANDLE, NULL, NULL);

    // Cleanup
    csFreeMem(device, output_mem);
    csFreeMem(device, input_mem);
    csCloseCSx(device);
    csShutdown();

    return status;
}
```

### Asynchronous Compute with Events

```c
#include <cs_api.h>

int run_async_compute(void) {
    csStatus status;
    csCSxHandle device;

    csInit();
    csOpenCSx("/dev/nvme0n1", &device);

    // Allocate buffers (same as sync example)
    csMemDesc desc = { .type = CS_MEM_TYPE_AFDM, .size = 4096 };
    csMemHandle mem;
    void *buffer;
    csAllocMem(device, &desc, &mem, &buffer);

    // Create event for completion notification
    csEventHandle event;
    csCreateEvent(device, &event);

    // Setup compute descriptor
    csCSFId csf_id;
    csGetCSFId(device, 0, &csf_id);

    csComputeDesc compute = {
        .csf_id = csf_id,
        .input_buffer = buffer,
        .input_size = 4096,
        .output_buffer = buffer,
        .output_size = 4096,
    };

    // Async execution (non-blocking)
    status = csQueueComputeRequest(device, &compute, event, NULL, NULL);

    if (status == CS_STATUS_PENDING) {
        // Do other work while compute runs...

        // Wait for completion (5 second timeout)
        csStatus result;
        status = csWaitEvent(device, event, 5000, &result);

        if (status == CS_STATUS_SUCCESS) {
            printf("Compute completed with result: %d\n", result);
        } else if (status == CS_STATUS_ERROR_TIMEOUT) {
            printf("Compute timed out!\n");
        }
    }

    // Cleanup
    csDestroyEvent(device, event);
    csFreeMem(device, mem);
    csCloseCSx(device);
    csShutdown();

    return status;
}
```

### Asynchronous Compute with Callback

```c
#include <cs_api.h>
#include <stdio.h>

// Callback function
void compute_done(csStatus status, void *user_data) {
    int *completed = (int *)user_data;
    printf("Compute completed with status: %d\n", status);
    *completed = 1;
}

int run_callback_compute(void) {
    csCSxHandle device;
    volatile int completed = 0;

    csInit();
    csOpenCSx("/dev/nvme0n1", &device);

    // Setup buffers and compute descriptor...
    csMemDesc desc = { .type = CS_MEM_TYPE_AFDM, .size = 4096 };
    csMemHandle mem;
    void *buffer;
    csAllocMem(device, &desc, &mem, &buffer);

    csCSFId csf_id;
    csGetCSFId(device, 0, &csf_id);

    csComputeDesc compute = {
        .csf_id = csf_id,
        .input_buffer = buffer,
        .input_size = 4096,
        .output_buffer = buffer,
        .output_size = 4096,
    };

    // Execute with callback
    csQueueComputeRequest(device, &compute, CS_INVALID_HANDLE,
                          compute_done, (void *)&completed);

    // Wait for callback
    while (!completed) {
        // Poll or do other work
    }

    // Cleanup
    csFreeMem(device, mem);
    csCloseCSx(device);
    csShutdown();

    return 0;
}
```

### Device Discovery

```c
#include <cs_api.h>
#include <stdio.h>
#include <stdlib.h>

void list_devices(void) {
    csInit();

    // Query device count
    uint32_t count = 0;
    csQueryCSxList(NULL, &count);

    if (count == 0) {
        printf("No CS devices found.\n");
        printf("Set LIBCS_DEVICE_URI environment variable.\n");
        csShutdown();
        return;
    }

    // Allocate and query device info
    csDeviceInfo *devices = malloc(count * sizeof(csDeviceInfo));
    csQueryCSxList(devices, &count);

    printf("Found %u CS device(s):\n\n", count);
    for (uint32_t i = 0; i < count; i++) {
        printf("Device %u:\n", i);
        printf("  URI:          %s\n", devices[i].uri);
        printf("  Vendor:       %s\n", devices[i].vendor);
        printf("  Model:        %s\n", devices[i].model);
        printf("  Max Programs: %u\n", devices[i].max_programs);
        printf("  Max Memory:   %u MiB\n", devices[i].max_memory_mb);
        printf("\n");
    }

    free(devices);
    csShutdown();
}
```

## Design Details

### Handle Encoding

Handles are 64-bit values encoding type, generation, and index:

```
┌────────┬─────────────────────────┬────────────────────────────────┐
│ Type   │ Generation              │ Index                          │
│ 8 bits │ 24 bits                 │ 32 bits                        │
└────────┴─────────────────────────┴────────────────────────────────┘
 [63:56]   [55:32]                   [31:0]
```

- **Type**: Distinguishes device/memory/event handles
- **Generation**: Prevents use-after-free (increments on reallocation)
- **Index**: Index into internal handle table

### Thread Safety

| Component | Mechanism |
|-----------|-----------|
| Library init | `pthread_mutex_t init_lock` |
| Device table | `pthread_mutex_t devices_lock` |
| Memory table | `pthread_mutex_t allocations_lock` |
| Event table | `pthread_mutex_t events_lock` |
| Event status | `atomic_int` for lock-free polling |
| Per-device ops | `pthread_mutex_t` per device |

### Resource Limits

| Resource | Limit | Configurable |
|----------|-------|--------------|
| Devices | 64 | CS_MAX_DEVICES |
| Memory allocations | 1024 | CS_MAX_ALLOCATIONS |
| Events | 256 | CS_MAX_EVENTS |
| Backends | 8 | Hardcoded |

### Backend Interface

To implement a new backend:

```c
#include "cs_backend.h"

static csStatus my_backend_init(void) { ... }
static csStatus my_backend_dev_open(const char *uri,
                                    struct cs_backend_ctx **ctx) { ... }
// ... implement other operations ...

static const cs_backend_ops_t my_backend_ops = {
    .name = "my_backend",
    .init = my_backend_init,
    .shutdown = my_backend_shutdown,
    .dev_query = my_backend_dev_query,
    .dev_open = my_backend_dev_open,
    .dev_close = my_backend_dev_close,
    .dev_get_info = my_backend_dev_get_info,
    .mem_alloc = my_backend_mem_alloc,
    .mem_free = my_backend_mem_free,
    .prog_load = my_backend_prog_load,
    .prog_activate = my_backend_prog_activate,
    .prog_deactivate = my_backend_prog_deactivate,
    .compute_execute = my_backend_compute_execute,
    .poll = my_backend_poll,
};

void my_backend_register(void) {
    cs_backend_register(&my_backend_ops);
}
```

## Environment Variables

| Variable | Description | Default |
|----------|-------------|---------|
| `LIBCS_DEVICE_URI` | Device path for discovery | None |
| `LIBCS_BACKEND` | Backend selection | First registered |
| `LIBCS_DEBUG` | Log level (0=off, 1=error, 2=warn, 3=info) | 0 |
| `LIBCS_XNVME_BE` | xNVMe backend (spdk, linux, etc.) | Auto |

## Testing

### Unit Tests

```bash
# Run all tests
meson test -C build

# Run with verbose output
meson test -C build -v

# Run specific test
./build/test_cs_api
```

### Integration Tests

```bash
# Set device URI
export LIBCS_DEVICE_URI="/dev/nvme0n1"

# Enable debug logging
export LIBCS_DEBUG=2

# Run tests
./build/test_cs_api
```

### Test Coverage

| Test | Device Required | Description |
|------|-----------------|-------------|
| `test_version` | No | Library version string |
| `test_init_shutdown` | No | Library lifecycle |
| `test_device_discovery` | No | Query devices |
| `test_invalid_handle` | No | Handle validation |
| `test_compute_arg_helper` | No | Argument packing |
| `test_device_lifecycle` | Yes | Open/close device |
| `test_memory_allocation` | Yes | DMA memory |
| `test_event_lifecycle` | Yes | Event management |
| `test_compute_sync` | Yes | Sync compute |

## Troubleshooting

### Common Issues

**"xNVMe not found" during build**
```bash
# Ensure xNVMe is built in parent directory
cd ../xnvme
meson setup build -Dwith-spdk=disabled
meson compile -C build
cd ../libcs
meson setup build --wipe
```

**"No devices found" at runtime**
```bash
# Set device URI
export LIBCS_DEVICE_URI="/dev/nvme0n1"

# Verify device exists
ls -la /dev/nvme*
```

**"Permission denied" opening device**
```bash
# Run as root or add user to nvme group
sudo ./test_cs_api

# Or fix permissions
sudo chmod 666 /dev/nvme0n1
```

**"Backend error" during compute**
```bash
# Enable debug logging
export LIBCS_DEBUG=3

# Check if program is loaded and activated
# Ensure device supports CP command set
```

### Debug Logging

```bash
export LIBCS_DEBUG=3  # Maximum verbosity

# Log output:
# [CS:INFO] Initializing CS API library v1.0.0
# [CS:INFO] Registered backend: xnvme
# [CS:DEBUG] Allocated 4096 bytes at 0x7f8a00000000
```

## Backend Mapping

### xNVMe Backend

| CS API | xNVMe Function | NVMe Command |
|--------|----------------|--------------|
| `csOpenCSx` | `xnvme_dev_open()` | - |
| `csCloseCSx` | `xnvme_dev_close()` | - |
| `csAllocMem` | `xnvme_buf_alloc()` | - |
| `csFreeMem` | `xnvme_buf_free()` | - |
| `csLoadProgram` | `xnvme_cmd_cp_load()` | Admin 0x85 |
| `csActivateProgram` | `xnvme_cmd_cp_activate()` | Admin 0x88 |
| `csDeactivateProgram` | `xnvme_cmd_cp_activate()` | Admin 0x88 |
| `csQueueComputeRequest` | `xnvme_cmd_cp_execute_buf()` | IO 0x01 |

## License

BSD-3-Clause

```
SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
SPDX-License-Identifier: BSD-3-Clause
```
