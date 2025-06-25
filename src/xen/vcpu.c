/* Copyright Amazon.com Inc. or its affiliates.
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
VcpuOp(
    _In_ ULONG          Command,
    _In_ unsigned int   vcpu_id,
    _In_opt_ PVOID      Argument
    )
{
    return HYPERCALL(LONG_PTR, vcpu_op, 3, Command, vcpu_id, Argument);
}

_Check_return_
XEN_API
NTSTATUS
VcpuSetPeriodicTimer(
    _In_ unsigned int               vcpu_id,
    _In_opt_ PLARGE_INTEGER         Period
    )
{
    LONG_PTR                        rc;
    NTSTATUS                        status;

    if (Period != NULL) {
        struct vcpu_set_periodic_timer  op;

        op.period_ns = Period->QuadPart * 100ull;

        rc = VcpuOp(VCPUOP_set_periodic_timer, vcpu_id, &op);
    } else {
        rc = VcpuOp(VCPUOP_stop_periodic_timer, vcpu_id, NULL);
    }

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
VcpuRegisterVcpuInfo(
    _In_ unsigned int               vcpu_id,
    _In_ PFN_NUMBER                 Pfn,
    _In_ ULONG                      Offset
    )
{
    struct vcpu_register_vcpu_info  op;
    LONG_PTR                        rc;
    NTSTATUS                        status;

    op.mfn = (xen_pfn_t)Pfn;
    op.offset = Offset;
    op.rsvd = 0;

    //
    // NOTE: This has to be called on the CPU with the matching vcpu_id
    //       otherwise Xen will fail the hypercall with -EINVAL.
    //

    rc = VcpuOp(VCPUOP_register_vcpu_info, vcpu_id, &op);

    if (rc < 0) {
        ERRNO_TO_STATUS(-rc, status);
        goto fail1;
    }

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;

}
