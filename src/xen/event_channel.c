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

static LONG_PTR
EventChannelOp(
    _In_ ULONG  Command,
    _In_ PVOID  Argument
    )
{
    return HYPERCALL(LONG_PTR, event_channel_op, 2, Command, Argument);
}

_Check_return_
XEN_API
NTSTATUS
EventChannelSend(
    _In_ ULONG          LocalPort
    )
{
    struct evtchn_send  op;
    LONG_PTR            rc;
    NTSTATUS            status;

    op.port = LocalPort;

    rc = EventChannelOp(EVTCHNOP_send, &op);

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
EventChannelAllocateUnbound(
    _In_ USHORT                 Domain,
    _Out_ ULONG                 *LocalPort
    )
{
    struct evtchn_alloc_unbound op;
    LONG_PTR                    rc;
    NTSTATUS                    status;

    op.dom = DOMID_SELF;
    op.remote_dom = Domain;

    rc = EventChannelOp(EVTCHNOP_alloc_unbound, &op);

    if (rc < 0) {
        ERRNO_TO_STATUS(-rc, status);
        goto fail1;
    }

    *LocalPort = op.port;

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

_Check_return_
XEN_API
NTSTATUS
EventChannelBindInterDomain(
    _In_ USHORT                     RemoteDomain,
    _In_ ULONG                      RemotePort,
    _Out_ ULONG                     *LocalPort
    )
{
    struct evtchn_bind_interdomain  op;
    LONG_PTR                        rc;
    NTSTATUS                        status;

    op.remote_dom = RemoteDomain,
    op.remote_port = RemotePort;

    rc = EventChannelOp(EVTCHNOP_bind_interdomain, &op);

    if (rc < 0) {
        ERRNO_TO_STATUS(-rc, status);
        goto fail1;
    }

    *LocalPort = op.local_port;

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

_Check_return_
XEN_API
NTSTATUS
EventChannelBindVirq(
    _In_ ULONG              Virq,
    _In_ unsigned int       vcpu_id,
    _Out_ ULONG             *LocalPort
    )
{
    struct evtchn_bind_virq op;
    LONG_PTR                rc;
    NTSTATUS                status;

    op.virq = Virq;
    op.vcpu = vcpu_id;

    rc = EventChannelOp(EVTCHNOP_bind_virq, &op);

    if (rc < 0) {
        ERRNO_TO_STATUS(-rc, status);
        goto fail1;
    }

    *LocalPort = op.port;

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

_Check_return_
XEN_API
NTSTATUS
EventChannelQueryInterDomain(
    _In_ ULONG              LocalPort,
    _Out_ USHORT            *RemoteDomain,
    _Out_ ULONG             *RemotePort
    )
{
    struct evtchn_status    op;
    LONG_PTR                rc;
    NTSTATUS                status;

    op.dom = DOMID_SELF;
    op.port = LocalPort;

    rc = EventChannelOp(EVTCHNOP_status, &op);

    if (rc < 0) {
        ERRNO_TO_STATUS(-rc, status);
        goto fail1;
    }

    status = STATUS_INVALID_PARAMETER;
    if (op.status != EVTCHNSTAT_interdomain)
        goto fail2;

    *RemoteDomain = op.u.interdomain.dom;
    *RemotePort = op.u.interdomain.port;

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
EventChannelClose(
    _In_ ULONG          LocalPort
    )
{
    struct evtchn_close op;
    LONG_PTR            rc;
    NTSTATUS            status;

    op.port = LocalPort;

    rc = EventChannelOp(EVTCHNOP_close, &op);

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
EventChannelExpandArray(
    _In_ PFN_NUMBER             Pfn
    )
{
    struct evtchn_expand_array  op;
    LONG_PTR                    rc;
    NTSTATUS                    status;

    op.array_gfn = Pfn;

    rc = EventChannelOp(EVTCHNOP_expand_array, &op);

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
EventChannelInitControl(
    _In_ PFN_NUMBER             Pfn,
    _In_ unsigned int           vcpu_id
    )
{
    struct evtchn_init_control  op;
    LONG_PTR                    rc;
    NTSTATUS                    status;

    op.control_gfn = Pfn;
    op.offset = 0;
    op.vcpu = vcpu_id;

    rc = EventChannelOp(EVTCHNOP_init_control, &op);

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
EventChannelReset(
    VOID
    )
{
    struct evtchn_reset op;
    LONG_PTR            rc;
    NTSTATUS            status;

    op.dom = DOMID_SELF;

    rc = EventChannelOp(EVTCHNOP_reset, &op);

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
EventChannelBindVirtualCpu(
    _In_ ULONG              LocalPort,
    _In_ unsigned int       vcpu_id
    )
{
    struct evtchn_bind_vcpu op;
    LONG_PTR                rc;
    NTSTATUS                status;

    op.port = LocalPort;
    op.vcpu = vcpu_id;

    rc = EventChannelOp(EVTCHNOP_bind_vcpu, &op);

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
EventChannelUnmask(
    _In_ ULONG              LocalPort
    )
{
    struct evtchn_unmask    op;
    LONG_PTR                rc;
    NTSTATUS                status;

    op.port = LocalPort;

    rc = EventChannelOp(EVTCHNOP_unmask, &op);

    if (rc < 0) {
        ERRNO_TO_STATUS(-rc, status);
        goto fail1;
    }

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}
