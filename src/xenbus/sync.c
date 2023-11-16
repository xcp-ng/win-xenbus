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

#include <ntddk.h>
#include <procgrp.h>
#include <stdarg.h>
#include <xen.h>

#include "sync.h"
#include "dbg_print.h"
#include "assert.h"
#include "util.h"

// Routines to capture all CPUs in a spinning state with interrupts
// disabled (so that we remain in a known code context).
// These routines are used for suspend/resume and live snapshot.

// The general sequence of steps is follows:
//
// - SyncCapture() is called on an arbitrary CPU. It must be called at
//   DISPATCH_LEVEL so it cannot be pre-empted and moved to another CPU.
//   It schedules a DPC on each of the other CPUs and spins until all
//   CPUs are executing the DPC, which will in-turn spin awaiting
//   further instruction.
//
// - SyncDisableInterrputs() instructs the DPC routines to all raise
//   to HIGH_LEVEL and disable interrupts for its CPU. It then raises
//   to HIGH_LEVEL itself, spins waiting for confirmation from each
//   DPC that it has disabled interrupts and then disables interrupts
//   itself.
//
//   NOTE: There is a back-off in trying to disable interrupts. It is
//         possible that CPU A is waiting for an IPI to CPU B to
//         complete, but CPU B is spinning with interrupts disabled.
//         Thus the DPC on CPU A will never make it to HIGH_LEVEL and
//         hence never get to disable interrupts. Thus if, while
//         spinning with interrupts disabled, one DPC notices that
//         another DPC has not made it, it briefly enables interrupts
//         and drops back down to DISPATCH_LEVEL before trying again.
//         This should allow any pending IPI to complete.
//
// - SyncEnableInterrupts() instructs the DPC routines to all enable
//   interrupts and drop back to DISPATCH_LEVEL before enabling
//   interrupts and dropping back to DISPATCH_LEVEL itself.
//
// - SyncRelease() instructs the DPC routines to exit, thus allowing
//   the scheduler to run on the other CPUs again. It spins until all
//   DPCs have completed and then returns.

typedef enum _SYNC_REQUEST {
    SYNC_REQUEST_NONE,
    SYNC_REQUEST_DISABLE_INTERRUPTS,
    SYNC_REQUEST_RUN_EARLY,
    SYNC_REQUEST_ENABLE_INTERRUPTS,
    SYNC_REQUEST_RUN_LATE,
    SYNC_REQUEST_EXIT,
} SYNC_REQUEST;

typedef struct  _SYNC_CONTEXT {
    PVOID           Argument;
    SYNC_CALLBACK   Early;
    SYNC_CALLBACK   Late;
    LONG            ProcessorCount;
    SYNC_REQUEST    Request;
    LONG            CompletionCount;
    PKDPC           Dpc;
} SYNC_CONTEXT, *PSYNC_CONTEXT;

static SYNC_CONTEXT     SyncContext;
static LONG             SyncOwner = -1;

#define XENBUS_SYNC_TAG 'CNYS'

static FORCEINLINE PVOID
__SyncAllocate(
    IN  ULONG   Length
    )
{
    return __AllocatePoolWithTag(NonPagedPool, Length, XENBUS_SYNC_TAG);
}

static FORCEINLINE VOID
__SyncFree(
    IN  PVOID   Buffer
    )
{
    __FreePoolWithTag(Buffer, XENBUS_SYNC_TAG);
}

static FORCEINLINE VOID
__SyncAcquire(
    IN  LONG    Index
    )
{
    LONG        Old;

    Old = InterlockedExchange(&SyncOwner, Index);
    ASSERT3U(Old, ==, -1);
}

static FORCEINLINE VOID
__SyncRelease(
    VOID
    )
{
    LONG    Old;
    LONG    Index;

    Index = KeGetCurrentProcessorNumberEx(NULL);

    Old = InterlockedExchange(&SyncOwner, -1);
    ASSERT3U(Old, ==, Index);
}

KDEFERRED_ROUTINE   SyncWorker;

#pragma intrinsic(_enable)
#pragma intrinsic(_disable)

#pragma warning(push)
#pragma warning(disable:28167) // Function changes IRQL and does not restore it before exit
#pragma warning(disable:28156) // Actual IRQL is inconsistent with required IRQL

static FORCEINLINE NTSTATUS
__SyncProcessorDisableInterrupts(
    VOID
    )
{
    PSYNC_CONTEXT   Context = &SyncContext;
    ULONG           Attempts;
    LONG            Old;
    LONG            New;
    NTSTATUS        status;

    (VOID) KfRaiseIrql(HIGH_LEVEL);
    status = STATUS_SUCCESS;

    InterlockedIncrement(&Context->CompletionCount);

    Attempts = 0;
    while (++Attempts <= 1000) {
        KeMemoryBarrier();

        if (Context->CompletionCount == Context->ProcessorCount)
            break;

        _mm_pause();
    }

    do {
        Old = Context->CompletionCount;
        New = Old - 1;

        if (Old == Context->ProcessorCount)
            break;
    } while (InterlockedCompareExchange(&Context->CompletionCount, New, Old) != Old);

    if (Old < Context->ProcessorCount) {
#pragma prefast(suppress:28138) // Use constant rather than variable
        KeLowerIrql(DISPATCH_LEVEL);
        status = STATUS_UNSUCCESSFUL;
    }

    if (NT_SUCCESS(status))
        _disable();

    return status;
}

static FORCEINLINE VOID
__SyncProcessorRunEarly(
    IN  ULONG       Index
    )
{
    PSYNC_CONTEXT   Context = &SyncContext;

    if (Context->Early != NULL)
        Context->Early(Context->Argument, Index);

    InterlockedIncrement(&Context->CompletionCount);
}

static FORCEINLINE VOID
__SyncProcessorEnableInterrupts(
    VOID
    )
{
    PSYNC_CONTEXT   Context = &SyncContext;

    _enable();

#pragma prefast(suppress:28138) // Use constant rather than variable
    KeLowerIrql(DISPATCH_LEVEL);

    InterlockedIncrement(&Context->CompletionCount);
}

static FORCEINLINE VOID
__SyncProcessorRunLate(
    IN  ULONG       Index
    )
{
    PSYNC_CONTEXT   Context = &SyncContext;

    if (Context->Late != NULL)
        Context->Late(Context->Argument, Index);

    InterlockedIncrement(&Context->CompletionCount);
}

static FORCEINLINE VOID
__SyncWait(
    VOID
    )
{
    PSYNC_CONTEXT   Context = &SyncContext;

    for (;;) {
        KeMemoryBarrier();

        if (Context->CompletionCount == Context->ProcessorCount)
            break;

        _mm_pause();
    }
}

VOID
#pragma prefast(suppress:28166) // Function does not restore IRQL
SyncWorker(
    IN  PKDPC           Dpc,
    IN  PVOID           _Context,
    IN  PVOID           Argument1,
    IN  PVOID           Argument2
    )
{
    PSYNC_CONTEXT       Context = &SyncContext;
    ULONG               Index;
    PROCESSOR_NUMBER    ProcNumber = { 0 };
    SYNC_REQUEST        Request;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(_Context);
    UNREFERENCED_PARAMETER(Argument1);
    UNREFERENCED_PARAMETER(Argument2);

    Index = KeGetCurrentProcessorNumberEx(&ProcNumber);

    ASSERT(SyncOwner >= 0 && Index != (ULONG)SyncOwner);

    Trace("====> (%u:%u)\n", ProcNumber.Group, ProcNumber.Number);
    InterlockedIncrement(&Context->CompletionCount);

    Request = SYNC_REQUEST_NONE;
    for (;;) {
        NTSTATUS status;

        KeMemoryBarrier();

        if (Context->Request == SYNC_REQUEST_EXIT)
            break;

        if (Context->Request == Request) {
            _mm_pause();
            continue;
        }

        status = STATUS_SUCCESS;

        switch (Context->Request) {
        case SYNC_REQUEST_DISABLE_INTERRUPTS:
            status = __SyncProcessorDisableInterrupts();
            break;

        case SYNC_REQUEST_RUN_EARLY:
            __SyncProcessorRunEarly(Index);
            break;

        case SYNC_REQUEST_ENABLE_INTERRUPTS:
            __SyncProcessorEnableInterrupts();
            break;

        case SYNC_REQUEST_RUN_LATE:
            __SyncProcessorRunLate(Index);
            break;

        default:
            break;
        }

        if (NT_SUCCESS(status))
            Request = Context->Request;
    }

    Trace("<==== (%u:%u)\n", ProcNumber.Group, ProcNumber.Number);
    InterlockedIncrement(&Context->CompletionCount);
}

__drv_maxIRQL(DISPATCH_LEVEL)
__drv_raisesIRQL(DISPATCH_LEVEL)
VOID
SyncCapture(
    IN  PVOID           Argument OPTIONAL,
    IN  SYNC_CALLBACK   Early OPTIONAL,
    IN  SYNC_CALLBACK   Late OPTIONAL
    )
{
    PSYNC_CONTEXT       Context = &SyncContext;
    LONG                Index;
    PROCESSOR_NUMBER    ProcNumber;
    USHORT              Group;
    UCHAR               Number;

    ASSERT3U(KeGetCurrentIrql(), ==, DISPATCH_LEVEL);

    Index = KeGetCurrentProcessorNumberEx(&ProcNumber);
    __SyncAcquire(Index);

    Group = ProcNumber.Group;
    Number = ProcNumber.Number;

    Trace("====> (%u:%u)\n", Group, Number);

    ASSERT(IsZeroMemory(Context, sizeof(SYNC_CONTEXT)));

    Context->Argument = Argument;
    Context->Early = Early;
    Context->Late = Late;

    Context->CompletionCount = 0;
    KeMemoryBarrier();

    Context->ProcessorCount = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
    Context->Dpc = __SyncAllocate(Context->ProcessorCount * sizeof(KDPC));
    ASSERT(Context->Dpc != NULL);

    for (Index = 0; Index < Context->ProcessorCount; Index++) {
        PKDPC       Dpc = &Context->Dpc[Index];
        NTSTATUS    status;

        status = KeGetProcessorNumberFromIndex(Index, &ProcNumber);
        ASSERT(NT_SUCCESS(status));

        if (ProcNumber.Group == Group &&
            ProcNumber.Number == Number)
            continue;

        KeInitializeDpc(Dpc, SyncWorker, NULL);
        KeSetTargetProcessorDpcEx(Dpc, &ProcNumber);
        KeInsertQueueDpc(Dpc, NULL, NULL);
    }

    KeMemoryBarrier();

    InterlockedIncrement(&Context->CompletionCount);
    __SyncWait();

    Trace("<==== (%u:%u)\n", Group, Number);
}

__drv_requiresIRQL(DISPATCH_LEVEL)
__drv_setsIRQL(HIGH_LEVEL)
VOID
SyncDisableInterrupts(
    VOID
    )
{
    PSYNC_CONTEXT   Context = &SyncContext;
    NTSTATUS        status;

    Trace("====>\n");

    ASSERT(SyncOwner >= 0);

    Context->CompletionCount = 0;
    KeMemoryBarrier();

    Context->Request = SYNC_REQUEST_DISABLE_INTERRUPTS;
    KeMemoryBarrier();

    for (;;) {
        status = __SyncProcessorDisableInterrupts();
        if (NT_SUCCESS(status))
            break;

        LogPrintf(LOG_LEVEL_WARNING, "SYNC: RE-TRY\n");
    }
}

__drv_requiresIRQL(HIGH_LEVEL)
VOID
SyncRunEarly(
    )
{
    PSYNC_CONTEXT   Context = &SyncContext;

    ASSERT(SyncOwner >= 0);

    Context->CompletionCount = 0;
    KeMemoryBarrier();

    __SyncProcessorRunEarly(SyncOwner);

    Context->Request = SYNC_REQUEST_RUN_EARLY;
    KeMemoryBarrier();

    __SyncWait();
}

__drv_requiresIRQL(HIGH_LEVEL)
__drv_setsIRQL(DISPATCH_LEVEL)
VOID
SyncEnableInterrupts(
    )
{
    PSYNC_CONTEXT   Context = &SyncContext;

    ASSERT(SyncOwner >= 0);

    Context->CompletionCount = 0;
    KeMemoryBarrier();

    __SyncProcessorEnableInterrupts();

    Context->Request = SYNC_REQUEST_ENABLE_INTERRUPTS;
    KeMemoryBarrier();

    __SyncWait();

    Trace("<====\n");
}

__drv_requiresIRQL(DISPATCH_LEVEL)
VOID
SyncRunLate(
    )
{
    PSYNC_CONTEXT   Context = &SyncContext;

    ASSERT(SyncOwner >= 0);

    Context->CompletionCount = 0;
    KeMemoryBarrier();

    __SyncProcessorRunLate(SyncOwner);

    Context->Request = SYNC_REQUEST_RUN_LATE;
    KeMemoryBarrier();

    __SyncWait();
}


__drv_requiresIRQL(DISPATCH_LEVEL)
VOID
#pragma prefast(suppress:28167) // Function changes IRQL
SyncRelease(
    VOID
    )
{
    PSYNC_CONTEXT   Context = &SyncContext;

    Trace("====>\n");

    ASSERT(SyncOwner >= 0);

    Context->CompletionCount = 0;
    KeMemoryBarrier();

    InterlockedIncrement(&Context->CompletionCount);

    Context->Request = SYNC_REQUEST_EXIT;
    KeMemoryBarrier();

    __SyncWait();

    __SyncFree(Context->Dpc);
    RtlZeroMemory(Context, sizeof(SYNC_CONTEXT));

    __SyncRelease();

    Trace("<====\n");
}

#pragma warning(pop)
