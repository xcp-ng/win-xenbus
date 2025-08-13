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

static FORCEINLINE LONG_PTR
MemoryOp(
    _In_ ULONG  Command,
    _In_ PVOID  Argument
    )
{
    return HYPERCALL(LONG_PTR, memory_op, 2, Command, Argument);
}

_Check_return_
XEN_API
NTSTATUS
MemoryAddToPhysmap(
    _In_ PFN_NUMBER             Pfn,
    _In_ ULONG                  Space,
    _In_ ULONG_PTR              Offset
    )
{
    struct xen_add_to_physmap   op;
    LONG_PTR                    rc;
    NTSTATUS                    status;

    op.domid = DOMID_SELF;
    op.space = Space;
    op.idx = Offset;
    op.gpfn = (xen_pfn_t)Pfn;

    rc = MemoryOp(XENMEM_add_to_physmap, &op);

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
MemoryRemoveFromPhysmap(
    _In_ PFN_NUMBER                 Pfn
    )
{
    struct xen_remove_from_physmap  op;
    LONG_PTR                        rc;
    NTSTATUS                        status;

    op.domid = DOMID_SELF;
    op.gpfn = (xen_pfn_t)Pfn;

    rc = MemoryOp(XENMEM_remove_from_physmap, &op);

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
MemoryDecreaseReservation(
    _In_ ULONG                      Order,
    _In_ ULONG                      Count,
    _In_ PPFN_NUMBER                PfnArray,
    _Out_ PULONG                    Result
    )
{
    struct xen_memory_reservation   op;
    LONG_PTR                        rc;
    NTSTATUS                        status;

    set_xen_guest_handle(op.extent_start, PfnArray);
    op.extent_order = Order;
    op.mem_flags = 0;
    op.domid = DOMID_SELF;
    op.nr_extents = Count;

    rc = MemoryOp(XENMEM_decrease_reservation, &op);

    if (rc < 0) {
        ERRNO_TO_STATUS(-rc, status);
        goto fail1;
    }

    *Result = (ULONG)rc;

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

_Check_return_
XEN_API
NTSTATUS
MemoryPopulatePhysmap(
    _In_ ULONG                      Order,
    _In_ ULONG                      Count,
    _In_ PPFN_NUMBER                PfnArray,
    _Out_ PULONG                    Result
    )
{
    struct xen_memory_reservation   op;
    LONG_PTR                        rc;
    NTSTATUS                        status;

    set_xen_guest_handle(op.extent_start, PfnArray);
    op.extent_order = Order;
    op.mem_flags = 0;
    op.domid = DOMID_SELF;
    op.nr_extents = Count;

    rc = MemoryOp(XENMEM_populate_physmap, &op);

    if (rc < 0) {
        ERRNO_TO_STATUS(-rc, status);
        goto fail1;
    }

    *Result = (ULONG)rc;

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}
