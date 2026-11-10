/* Copyright (c) Xen Project.
 * Copyright (c) Cloud Software Group, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms,
 * with or without modification, are permitted provided
 * that the following conditions are met:
 *
 * *   Redistributions of source code must retain the above
 *     copyright notice, this list of conditions and the
 *     following disclaimer.
 * *   Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the
 *     following disclaimer in the documentation and/or other
 *     materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#define XEN_API __declspec(dllexport)

#include <ntddk.h>
#include <xen.h>

#include "hypercall.h"
#include "dbg_print.h"
#include "assert.h"
#include "util.h"

#pragma warning(push)
#pragma warning(disable:4127)   // conditional expression is constant

#define XEN_GNTTAB_TAG  'TNGX'

// Most of the GNTST_* values don't have meaningful NTSTATUS counterparts,
// this macro translates those that do.
#define GNTST_TO_STATUS(_gntst, _status)                    \
        do {                                                \
            switch (_gntst) {                               \
            case GNTST_okay:                                \
                _status = STATUS_SUCCESS;                   \
                break;                                      \
                                                            \
            case GNTST_bad_handle:                          \
                _status = STATUS_INVALID_HANDLE;            \
                break;                                      \
                                                            \
            case GNTST_permission_denied:                   \
                _status = STATUS_ACCESS_DENIED;             \
                break;                                      \
                                                            \
            case GNTST_eagain:                              \
                _status = STATUS_RETRY;                     \
                break;                                      \
                                                            \
            default:                                        \
                _status = STATUS_UNSUCCESSFUL;              \
                break;                                      \
            }                                               \
        } while (FALSE)

#pragma warning(pop)

static LONG_PTR
GrantTableOp(
    _In_ ULONG  Command,
    _In_ PVOID  Argument,
    _In_ ULONG  Count
    )
{
    return HYPERCALL(LONG_PTR, grant_table_op, 3, Command, Argument, Count);
}

_Check_return_
XEN_API
NTSTATUS
GrantTableSetVersion(
    _In_ uint32_t               Version
    )
{
    struct gnttab_set_version   op;
    LONG_PTR                    rc;
    NTSTATUS                    status;

    op.version = Version;

    rc = GrantTableOp(GNTTABOP_set_version, &op, 1);

    if (rc < 0) {
        ERRNO_TO_STATUS(-rc, status);
        goto fail1;
    }

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

_Check_return_
XEN_API
NTSTATUS
GrantTableGetVersion(
    _Out_ uint32_t              *Version
    )
{
    struct gnttab_get_version   op;
    LONG_PTR                    rc;
    NTSTATUS                    status;

    op.dom = DOMID_SELF;

    rc = GrantTableOp(GNTTABOP_get_version, &op, 1);

    if (rc < 0) {
        ERRNO_TO_STATUS(-rc, status);
        goto fail1;
    }

    *Version = op.version;

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

_Check_return_
XEN_API
NTSTATUS
GrantTableCopy(
    _In_ struct gnttab_copy op[],
    _In_ ULONG              Count
    )
{
    LONG_PTR                rc;
    NTSTATUS                status;

    rc = GrantTableOp(GNTTABOP_copy, &op[0], Count);

    if (rc < 0) {
        ERRNO_TO_STATUS(-rc, status);
        goto fail1;
    }

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

_Check_return_
XEN_API
NTSTATUS
GrantTableMapForeignPages(
    _In_ USHORT                          Domain,
    _In_ ULONG                           NumberPages,
    _In_reads_(NumberPages) PULONG       GrantRefs,
    _In_ PHYSICAL_ADDRESS                StartAddress,
    _In_ BOOLEAN                         ReadOnly,
    _Out_writes_(NumberPages) PULONG     Handles,
    _Out_writes_(NumberPages) PNTSTATUS  Results
    )
{
    struct gnttab_map_grant_ref          *ops;
    LONG_PTR                             rc;
    NTSTATUS                             status;
    ULONG                                i;
    ULONG                                fail_count;

    status = STATUS_INVALID_PARAMETER;
    if (NumberPages == 0 || NumberPages > XEN_GNTTAB_MAP_BATCH_MAX)
        goto fail1;

    for (i = 0; i < NumberPages; i++)
        Handles[i] = XEN_GNTTAB_INVALID_HANDLE;

    status = STATUS_NO_MEMORY;
    ops = __AllocatePoolWithTag(NonPagedPool,
                                NumberPages * sizeof(*ops),
                                XEN_GNTTAB_TAG);
    if (ops == NULL)
        goto fail2;

    for (i = 0; i < NumberPages; i++) {
        ops[i].dom = Domain;
        ops[i].ref = GrantRefs[i];
        ops[i].flags = GNTMAP_host_map;
        if (ReadOnly)
            ops[i].flags |= GNTMAP_readonly;

        ops[i].host_addr = StartAddress.QuadPart + ((ULONGLONG)i << PAGE_SHIFT);
    }

    rc = GrantTableOp(GNTTABOP_map_grant_ref, ops, NumberPages);

    if (rc != 0) {
        Error("map_grant_ref: %u ops at 0x%08x%08x failed (%lld)\n",
              NumberPages,
              (ULONG)(StartAddress.QuadPart >> 32),
              (ULONG)StartAddress.QuadPart,
              rc);

        // Possible Xen errors during a hypercall:
        // EFAULT: Xen failed to access the output buffer. Some pages may
        //         have been processed, but the state is unknown.
        // EINVAL: invalid arguments
        // ENOSYS: unsupported hypercall
        // EPERM: caller not running at ring 0
        // All of these indicate either an unsafe state or corrupted
        // executing environment.
        BUG("map hypercall failed");
    }

    fail_count = 0;
    for (i = 0; i < NumberPages; i++) {
        GNTST_TO_STATUS(ops[i].status, Results[i]);

        if (NT_SUCCESS(Results[i])) {
            Handles[i] = ops[i].handle;
        } else {
            if (fail_count == 0) {
                Warning("op[%u] %hu:%u -> 0x%08x%08x failed (%hd)\n",
                        i,
                        ops[i].dom, ops[i].ref,
                        (ULONG)(ops[i].host_addr >> 32),
                        (ULONG)ops[i].host_addr,
                        ops[i].status);
            }

            fail_count++;
        }
    }

    if (fail_count > 0)
        Warning("%u of %u ops failed\n", fail_count, NumberPages);

    __FreePoolWithTag(ops, XEN_GNTTAB_TAG);
    return STATUS_SUCCESS;

fail2:
    Error("fail2\n");

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

_Check_return_
XEN_API
NTSTATUS
GrantTableUnmapForeignPage(
    _In_ ULONG                    Handle,
    _In_ PHYSICAL_ADDRESS         Address
    )
{
    struct gnttab_unmap_grant_ref op;
    LONG_PTR                      rc;
    NTSTATUS                      status;

    RtlZeroMemory(&op, sizeof(op));
    op.handle = Handle;
    op.host_addr = Address.QuadPart;

    rc = GrantTableOp(GNTTABOP_unmap_grant_ref, &op, 1);

    if (rc < 0) {
        ERRNO_TO_STATUS(-rc, status);
        goto fail1;
    }

    if (op.status != GNTST_okay) {
        Warning("0x%08x%08x failed (%hd)\n",
                Address.HighPart,
                Address.LowPart,
                op.status);

        GNTST_TO_STATUS(op.status, status);
        goto fail2;
    }

    return STATUS_SUCCESS;

fail2:
    Error("fail2\n");

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

_Check_return_
XEN_API
NTSTATUS
GrantTableUnmapForeignPages(
    _In_ ULONG                           NumberPages,
    _In_reads_(NumberPages) PULONG       Handles,
    _In_ PHYSICAL_ADDRESS                StartAddress,
    _Out_writes_(NumberPages) PNTSTATUS  Results
    )
{
    struct gnttab_unmap_grant_ref        *ops;
    LONG_PTR                             rc;
    NTSTATUS                             status;
    ULONG                                i;
    ULONG                                count;
    ULONG                                fail_count;

    status = STATUS_INVALID_PARAMETER;
    if (NumberPages == 0 || NumberPages > XEN_GNTTAB_MAP_BATCH_MAX)
        goto fail1;

    count = 0;
    for (i = 0; i < NumberPages; i++) {
        if (Handles[i] != XEN_GNTTAB_INVALID_HANDLE)
            count++;
    }

    if (count == 0)
        goto done;

    status = STATUS_NO_MEMORY;
    ops = __AllocatePoolWithTag(NonPagedPool,
                                count * sizeof(*ops),
                                XEN_GNTTAB_TAG);
    if (ops == NULL)
        goto fail2;

    count = 0;
    for (i = 0; i < NumberPages; i++) {
        if (Handles[i] == XEN_GNTTAB_INVALID_HANDLE)
            continue;

        ops[count].handle = Handles[i];
        ops[count].host_addr = StartAddress.QuadPart + ((ULONGLONG)i << PAGE_SHIFT);
        count++;
    }

    rc = GrantTableOp(GNTTABOP_unmap_grant_ref, ops, count);

    if (rc != 0) {
        Error("unmap_grant_ref: %u ops at 0x%08x%08x failed (%lld)\n",
              count,
              (ULONG)(StartAddress.QuadPart >> 32),
              (ULONG)StartAddress.QuadPart,
              rc);

        // See GrantTableMapForeignPages() for explanation why this is fatal
        BUG("unmap hypercall failed");
    }

    count = 0;
    fail_count = 0;
    for (i = 0; i < NumberPages; i++) {
        if (Handles[i] == XEN_GNTTAB_INVALID_HANDLE) {
            Results[i] = STATUS_SUCCESS;
            continue;
        }

        GNTST_TO_STATUS(ops[count].status, Results[i]);

        if (!NT_SUCCESS(Results[i])) {
            if (fail_count == 0) {
                Warning("op[%u] 0x%08x%08x failed (%hd)\n",
                        i,
                        (ULONG)(ops[count].host_addr >> 32),
                        (ULONG)ops[count].host_addr,
                        ops[count].status);
            }

            fail_count++;
        }

        count++;
    }

    if (fail_count > 0)
        Warning("%u of %u ops failed\n", fail_count, count);

    __FreePoolWithTag(ops, XEN_GNTTAB_TAG);

done:
    return STATUS_SUCCESS;

fail2:
    Error("fail2 (%08x)\n", status);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

_Check_return_
XEN_API
NTSTATUS
GrantTableQuerySize(
    _Out_opt_ uint32_t          *Current,
    _Out_opt_ uint32_t          *Maximum
    )
{
    struct gnttab_query_size    op;
    LONG_PTR                    rc;
    NTSTATUS                    status;

    op.dom = DOMID_SELF;

    rc = GrantTableOp(GNTTABOP_query_size, &op, 1);

    if (rc < 0) {
        ERRNO_TO_STATUS(-rc, status);
        goto fail1;
    }

    status = STATUS_UNSUCCESSFUL;
    if (op.status != GNTST_okay)
        goto fail2;

    if (Current != NULL)
        *Current = op.nr_frames;

    if (Maximum != NULL)
        *Maximum = op.max_nr_frames;

    return STATUS_SUCCESS;

fail2:
    Error("fail2\n");

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}
