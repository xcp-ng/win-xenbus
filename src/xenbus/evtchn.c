/* Copyright (c) Citrix Systems Inc.
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
#include <stdarg.h>
#include <xen.h>
#include <util.h>

#include "evtchn.h"
#include "evtchn_2l.h"
#include "evtchn_fifo.h"
#include "fdo.h"
#include "hash_table.h"
#include "registry.h"
#include "dbg_print.h"
#include "assert.h"

typedef struct _XENBUS_EVTCHN_UNBOUND_PARAMETERS {
    USHORT  RemoteDomain;
} XENBUS_EVTCHN_UNBOUND_PARAMETERS, *PXENBUS_EVTCHN_UNBOUND_PARAMETERS;

typedef struct _XENBUS_EVTCHN_INTER_DOMAIN_PARAMETERS {
    USHORT  RemoteDomain;
    ULONG   RemotePort;
} XENBUS_EVTCHN_INTER_DOMAIN_PARAMETERS, *PXENBUS_EVTCHN_INTER_DOMAIN_PARAMETERS;

typedef struct _XENBUS_EVTCHN_VIRQ_PARAMETERS {
    ULONG   Index;
} XENBUS_EVTCHN_VIRQ_PARAMETERS, *PXENBUS_EVTCHN_VIRQ_PARAMETERS;

#pragma warning(push)
#pragma warning(disable:4201)   // nonstandard extension used : nameless struct/union

typedef struct _XENBUS_EVTCHN_PARAMETERS {
    union {
        XENBUS_EVTCHN_UNBOUND_PARAMETERS       Unbound;
        XENBUS_EVTCHN_INTER_DOMAIN_PARAMETERS  InterDomain;
        XENBUS_EVTCHN_VIRQ_PARAMETERS          Virq;
    };
} XENBUS_EVTCHN_PARAMETERS, *PXENBUS_EVTCHN_PARAMETERS;

#pragma warning(pop)

#define XENBUS_EVTCHN_CHANNEL_MAGIC 'NAHC'

struct _XENBUS_EVTCHN_CHANNEL {
    ULONG                       Magic;
    KSPIN_LOCK                  Lock;
    LIST_ENTRY                  ListEntry;
    LIST_ENTRY                  PendingListEntry;
    PVOID                       Caller;
    PKSERVICE_ROUTINE           Callback;
    PVOID                       Argument;
    BOOLEAN                     Active; // Must be tested at >= DISPATCH_LEVEL
    XENBUS_EVTCHN_TYPE          Type;
    XENBUS_EVTCHN_PARAMETERS    Parameters;
    BOOLEAN                     Mask;
    ULONG                       LocalPort;
    ULONG                       Cpu;
    BOOLEAN                     Closed;
};

struct _XENBUS_EVTCHN_CONTEXT {
    PXENBUS_FDO                     Fdo;
    KSPIN_LOCK                      Lock;
    LONG                            References;
    PXENBUS_INTERRUPT               LevelSensitiveInterrupt;
    PXENBUS_INTERRUPT               LatchedInterrupt[MAXIMUM_PROCESSORS];
    KAFFINITY                       Affinity;
    XENBUS_SUSPEND_INTERFACE        SuspendInterface;
    PXENBUS_SUSPEND_CALLBACK        SuspendCallbackEarly;
    PXENBUS_SUSPEND_CALLBACK        SuspendCallbackLate;
    XENBUS_DEBUG_INTERFACE          DebugInterface;
    PXENBUS_DEBUG_CALLBACK          DebugCallback;
    XENBUS_SHARED_INFO_INTERFACE    SharedInfoInterface;
    PXENBUS_EVTCHN_ABI_CONTEXT      EvtchnTwoLevelContext;
    PXENBUS_EVTCHN_ABI_CONTEXT      EvtchnFifoContext;
    XENBUS_EVTCHN_ABI               EvtchnAbi;
    BOOLEAN                         UseEvtchnFifoAbi;
    PXENBUS_HASH_TABLE              Table;
    LIST_ENTRY                      List;
    LIST_ENTRY                      PendingList[MAXIMUM_PROCESSORS];
    KDPC                            Dpc[MAXIMUM_PROCESSORS];
};

#define XENBUS_EVTCHN_TAG  'CTVE'

static FORCEINLINE PVOID
__EvtchnAllocate(
    IN  ULONG   Length
    )
{
    return __AllocatePoolWithTag(NonPagedPool, Length, XENBUS_EVTCHN_TAG);
}

static FORCEINLINE VOID
__EvtchnFree(
    IN  PVOID   Buffer
    )
{
    ExFreePoolWithTag(Buffer, XENBUS_EVTCHN_TAG);
}

static NTSTATUS
EvtchnOpenFixed(
    IN  PXENBUS_EVTCHN_CHANNEL  Channel,
    IN  va_list                 Arguments
    )
{
    ULONG                       LocalPort;
    BOOLEAN                     Mask;

    LocalPort = va_arg(Arguments, ULONG);
    Mask = va_arg(Arguments, BOOLEAN);

    Channel->Mask = Mask;
    Channel->LocalPort = LocalPort;

    return STATUS_SUCCESS;
}

static NTSTATUS
EvtchnOpenUnbound(
    IN  PXENBUS_EVTCHN_CHANNEL  Channel,
    IN  va_list                 Arguments
    )
{
    USHORT                      RemoteDomain;
    BOOLEAN                     Mask;
    ULONG                       LocalPort;
    NTSTATUS                    status;

    RemoteDomain = va_arg(Arguments, USHORT);
    Mask = va_arg(Arguments, BOOLEAN);

    status = EventChannelAllocateUnbound(RemoteDomain, &LocalPort);
    if (!NT_SUCCESS(status))
        goto fail1;

    Channel->Parameters.Unbound.RemoteDomain = RemoteDomain;

    Channel->Mask = Mask;
    Channel->LocalPort = LocalPort;

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static NTSTATUS
EvtchnOpenInterDomain(
    IN  PXENBUS_EVTCHN_CHANNEL  Channel,
    IN  va_list                 Arguments
    )
{
    USHORT                      RemoteDomain;
    ULONG                       RemotePort;
    BOOLEAN                     Mask;
    ULONG                       LocalPort;
    NTSTATUS                    status;

    RemoteDomain = va_arg(Arguments, USHORT);
    RemotePort = va_arg(Arguments, ULONG);
    Mask = va_arg(Arguments, BOOLEAN);

    status = EventChannelBindInterDomain(RemoteDomain,
                                         RemotePort,
                                         &LocalPort);
    if (!NT_SUCCESS(status))
        goto fail1;

    Channel->Parameters.InterDomain.RemoteDomain = RemoteDomain;
    Channel->Parameters.InterDomain.RemotePort = RemotePort;

    Channel->Mask = Mask;
    Channel->LocalPort = LocalPort;

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static NTSTATUS
EvtchnOpenVirq(
    IN  PXENBUS_EVTCHN_CHANNEL  Channel,
    IN  va_list                 Arguments
    )
{
    ULONG                       Index;
    ULONG                       LocalPort;
    NTSTATUS                    status;

    Index = va_arg(Arguments, ULONG);

    status = EventChannelBindVirq(Index, &LocalPort);
    if (!NT_SUCCESS(status))
        goto fail1;

    Channel->Parameters.Virq.Index = Index;

    Channel->LocalPort = LocalPort;

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

extern USHORT
RtlCaptureStackBackTrace(
    __in        ULONG   FramesToSkip,
    __in        ULONG   FramesToCapture,
    __out       PVOID   *BackTrace,
    __out_opt   PULONG  BackTraceHash
    );

static PXENBUS_EVTCHN_CHANNEL
EvtchnOpen(
    IN  PINTERFACE          Interface,
    IN  XENBUS_EVTCHN_TYPE  Type,
    IN  PKSERVICE_ROUTINE   Callback,
    IN  PVOID               Argument OPTIONAL,
    ...
    )
{
    PXENBUS_EVTCHN_CONTEXT  Context = Interface->Context;
    va_list                 Arguments;
    PXENBUS_EVTCHN_CHANNEL  Channel;
    ULONG                   LocalPort;
    KIRQL                   Irql;
    NTSTATUS                status;

    KeRaiseIrql(DISPATCH_LEVEL, &Irql); // Prevent suspend

    Channel = __EvtchnAllocate(sizeof (XENBUS_EVTCHN_CHANNEL));

    status = STATUS_NO_MEMORY;
    if (Channel == NULL)
        goto fail1;

    Channel->Magic = XENBUS_EVTCHN_CHANNEL_MAGIC;

    (VOID) RtlCaptureStackBackTrace(1, 1, &Channel->Caller, NULL);    

    Channel->Type = Type;
    Channel->Callback = Callback;
    Channel->Argument = Argument;

    va_start(Arguments, Argument);
    switch (Type) {
    case XENBUS_EVTCHN_TYPE_FIXED:
        status = EvtchnOpenFixed(Channel, Arguments);
        break;

    case XENBUS_EVTCHN_TYPE_UNBOUND:
        status = EvtchnOpenUnbound(Channel, Arguments);
        break;

    case XENBUS_EVTCHN_TYPE_INTER_DOMAIN:
        status = EvtchnOpenInterDomain(Channel, Arguments);
        break;

    case XENBUS_EVTCHN_TYPE_VIRQ:
        status = EvtchnOpenVirq(Channel, Arguments);
        break;

    default:
        status = STATUS_INVALID_PARAMETER;
        break;
    }
    va_end(Arguments);

    if (!NT_SUCCESS(status))
        goto fail2;

    LocalPort = Channel->LocalPort;

    Trace("%u\n", LocalPort);

    InitializeListHead(&Channel->PendingListEntry);

    status = XENBUS_EVTCHN_ABI(PortEnable,
                               &Context->EvtchnAbi,
                               LocalPort);
    if (!NT_SUCCESS(status))
        goto fail3;

    status = HashTableAdd(Context->Table,
                          LocalPort,
                          (ULONG_PTR)Channel);
    if (!NT_SUCCESS(status))
        goto fail4;

    Channel->Active = TRUE;

    KeAcquireSpinLockAtDpcLevel(&Context->Lock);
    InsertTailList(&Context->List, &Channel->ListEntry);
    KeReleaseSpinLockFromDpcLevel(&Context->Lock);

    KeLowerIrql(Irql);

    KeInitializeSpinLock(&Channel->Lock);

    return Channel;

fail4:
    Error("fail4\n");

    XENBUS_EVTCHN_ABI(PortDisable,
                      &Context->EvtchnAbi,
                      LocalPort);

fail3:
    Error("fail3\n");

    ASSERT(IsListEmpty(&Channel->PendingListEntry));
    RtlZeroMemory(&Channel->PendingListEntry, sizeof (LIST_ENTRY));

    Channel->LocalPort = 0;
    Channel->Mask = FALSE;
    RtlZeroMemory(&Channel->Parameters, sizeof (XENBUS_EVTCHN_PARAMETERS));

    if (Channel->Type != XENBUS_EVTCHN_TYPE_FIXED)
        (VOID) EventChannelClose(LocalPort);

fail2:
    Error("fail2\n");

    Channel->Argument = NULL;
    Channel->Callback = NULL;
    Channel->Type = 0;

    Channel->Caller = NULL;

    Channel->Magic = 0;

    ASSERT(IsZeroMemory(Channel, sizeof (XENBUS_EVTCHN_CHANNEL)));
    __EvtchnFree(Channel);

fail1:
    Error("fail1 (%08x)\n", status);

    KeLowerIrql(Irql);

    return NULL;
}

static VOID
EvtchnReap(
    IN  PXENBUS_EVTCHN_CONTEXT  Context,
    IN  PXENBUS_EVTCHN_CHANNEL  Channel,
    IN  BOOLEAN                 Close
    )
{
    ULONG                       LocalPort = Channel->LocalPort;

    UNREFERENCED_PARAMETER(Context);

    Trace("%u\n", LocalPort);

    ASSERT(Channel->Closed);
    Channel->Closed = FALSE;

    RtlZeroMemory(&Channel->Lock, sizeof (KSPIN_LOCK));

    RemoveEntryList(&Channel->ListEntry);
    RtlZeroMemory(&Channel->ListEntry, sizeof (LIST_ENTRY));

    Channel->Cpu = 0;

    ASSERT(IsListEmpty(&Channel->PendingListEntry));
    RtlZeroMemory(&Channel->PendingListEntry, sizeof (LIST_ENTRY));

    Channel->LocalPort = 0;
    Channel->Mask = FALSE;
    RtlZeroMemory(&Channel->Parameters, sizeof (XENBUS_EVTCHN_PARAMETERS));

    if (Close && Channel->Type != XENBUS_EVTCHN_TYPE_FIXED)
        (VOID) EventChannelClose(LocalPort);

    Channel->Argument = NULL;
    Channel->Callback = NULL;
    Channel->Type = 0;

    Channel->Caller = NULL;

    Channel->Magic = 0;

    ASSERT(IsZeroMemory(Channel, sizeof (XENBUS_EVTCHN_CHANNEL)));
    __EvtchnFree(Channel);
}

static BOOLEAN
EvtchnPollCallback(
    IN  PVOID               Argument,
    IN  ULONG               LocalPort
    )
{
    PXENBUS_EVTCHN_CONTEXT  Context = Argument;
    ULONG                   Cpu;
    PXENBUS_EVTCHN_CHANNEL  Channel;
    BOOLEAN                 Pending;
    NTSTATUS                status;

    ASSERT3U(KeGetCurrentIrql(), >=, DISPATCH_LEVEL);
    Cpu = KeGetCurrentProcessorNumber();

    status = HashTableLookup(Context->Table,
                             LocalPort,
                             (PULONG_PTR)&Channel);
    if (!NT_SUCCESS(status))
        goto done;

    ASSERT3U(Channel->LocalPort, ==, LocalPort);

    Pending = !IsListEmpty(&Channel->PendingListEntry);

    if (!Pending)
        InsertTailList(&Context->PendingList[Cpu],
                       &Channel->PendingListEntry);

done:
    return FALSE;
}

static BOOLEAN
EvtchnPoll(
    IN  PXENBUS_EVTCHN_CONTEXT  Context,
    IN  ULONG                   Cpu,
    IN  PLIST_ENTRY             List
    )
{
    BOOLEAN                     DoneSomething;
    PLIST_ENTRY                 ListEntry;

    (VOID) XENBUS_EVTCHN_ABI(Poll,
                             &Context->EvtchnAbi,
                             Cpu,
                             EvtchnPollCallback,
                             Context);

    DoneSomething = FALSE;

    ListEntry = Context->PendingList[Cpu].Flink;
    while (ListEntry != &Context->PendingList[Cpu]) {
        PLIST_ENTRY             Next = ListEntry->Flink;
        PXENBUS_EVTCHN_CHANNEL  Channel;

        Channel = CONTAINING_RECORD(ListEntry,
                                    XENBUS_EVTCHN_CHANNEL,
                                    PendingListEntry);

        ASSERT3U(Channel->Magic, ==, XENBUS_EVTCHN_CHANNEL_MAGIC);

        KeMemoryBarrier();
        if (!Channel->Closed) {
            RemoveEntryList(&Channel->PendingListEntry);
            InitializeListHead(&Channel->PendingListEntry);

            if (Channel->Mask)
                XENBUS_EVTCHN_ABI(PortMask,
                                  &Context->EvtchnAbi,
                                  Channel->LocalPort);

            XENBUS_EVTCHN_ABI(PortAck,
                              &Context->EvtchnAbi,
                              Channel->LocalPort);

#pragma warning(suppress:6387)  // NULL argument
            DoneSomething |= Channel->Callback(NULL, Channel->Argument);
        } else if (List != NULL) {
            RemoveEntryList(&Channel->PendingListEntry);
            InsertTailList(List, &Channel->PendingListEntry);
        }

        ListEntry = Next;
    }

    return DoneSomething;
}

static VOID
EvtchnFlush(
    IN  PXENBUS_EVTCHN_CONTEXT  Context,
    IN  ULONG                   Cpu
    )
{
    LIST_ENTRY                  List;
    PXENBUS_INTERRUPT           Interrupt;
    KIRQL                       Irql;

    Interrupt = (Context->Affinity != 0) ? // Latched available
                Context->LatchedInterrupt[Cpu] :
                Context->LevelSensitiveInterrupt;

    InitializeListHead(&List);

    Irql = FdoAcquireInterruptLock(Context->Fdo, Interrupt);

    (VOID) EvtchnPoll(Context, Cpu, &List);

    FdoReleaseInterruptLock(Context->Fdo, Interrupt, Irql);

    while (!IsListEmpty(&List)) {
        PLIST_ENTRY             ListEntry;
        PXENBUS_EVTCHN_CHANNEL  Channel;

        ListEntry = RemoveHeadList(&List);
        ASSERT(ListEntry != &List);

        Channel = CONTAINING_RECORD(ListEntry,
                                    XENBUS_EVTCHN_CHANNEL,
                                    PendingListEntry);

        ASSERT3U(Channel->Magic, ==, XENBUS_EVTCHN_CHANNEL_MAGIC);

        InitializeListHead(&Channel->PendingListEntry);

        EvtchnReap(Context, Channel, TRUE);
    }
}

static
_Function_class_(KDEFERRED_ROUTINE)
_IRQL_requires_max_(DISPATCH_LEVEL)
_IRQL_requires_min_(DISPATCH_LEVEL)
_IRQL_requires_(DISPATCH_LEVEL)
_IRQL_requires_same_
VOID
EvtchnDpc(
    IN  PKDPC               Dpc,
    IN  PVOID               _Context,
    IN  PVOID               Argument1,
    IN  PVOID               Argument2
    )
{
    PXENBUS_EVTCHN_CONTEXT  Context = _Context;
    ULONG                   Cpu;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Argument1);
    UNREFERENCED_PARAMETER(Argument2);

    ASSERT3U(KeGetCurrentIrql(), >=, DISPATCH_LEVEL);
    Cpu = KeGetCurrentProcessorNumber();

    KeAcquireSpinLockAtDpcLevel(&Context->Lock);

    if (Context->References == 0)
        goto done;

    EvtchnFlush(Context, Cpu);

done:
    KeReleaseSpinLockFromDpcLevel(&Context->Lock);
}

static VOID
EvtchnTrigger(
    IN  PINTERFACE              Interface,
    IN  PXENBUS_EVTCHN_CHANNEL  Channel
    )
{
    PXENBUS_EVTCHN_CONTEXT      Context = Interface->Context;
    PKDPC                       Dpc;
    KIRQL                       Irql;
    ULONG                       Cpu;
    PXENBUS_INTERRUPT           Interrupt;
    BOOLEAN                     Pending;

    ASSERT3U(Channel->Magic, ==, XENBUS_EVTCHN_CHANNEL_MAGIC);

    KeAcquireSpinLock(&Channel->Lock, &Irql);
    Cpu = Channel->Cpu;
    KeReleaseSpinLock(&Channel->Lock, Irql);

    Interrupt = (Context->Affinity != 0) ? // Latched available
                Context->LatchedInterrupt[Cpu] :
                Context->LevelSensitiveInterrupt;

    Irql = FdoAcquireInterruptLock(Context->Fdo, Interrupt);

    Pending = !IsListEmpty(&Channel->PendingListEntry);

    if (!Pending)
        InsertTailList(&Context->PendingList[Cpu],
                       &Channel->PendingListEntry);

    FdoReleaseInterruptLock(Context->Fdo, Interrupt, Irql);

    if (Pending)
        return;

    Dpc = &Context->Dpc[Cpu];
    KeInsertQueueDpc(Dpc, NULL, NULL);
}

static NTSTATUS
EvtchnBind(
    IN  PINTERFACE              Interface,
    IN  PXENBUS_EVTCHN_CHANNEL  Channel,
    IN  ULONG                   Cpu
    )
{
    PXENBUS_EVTCHN_CONTEXT      Context = Interface->Context;
    ULONG                       LocalPort;
    unsigned int                vcpu_id;
    KIRQL                       Irql;
    NTSTATUS                    status;

    ASSERT3U(Channel->Magic, ==, XENBUS_EVTCHN_CHANNEL_MAGIC);

    status = STATUS_INVALID_PARAMETER;
    if (Cpu >= (ULONG)KeNumberProcessors)
        goto fail1;

    status = STATUS_NOT_SUPPORTED;
    if (~Context->Affinity & ((KAFFINITY)1 << Cpu))
        goto fail2;

    KeAcquireSpinLock(&Channel->Lock, &Irql);

    if (!Channel->Active)
        goto done;

    if (Channel->Cpu == Cpu)
        goto done;

    LocalPort = Channel->LocalPort;
    vcpu_id = SystemVirtualCpuIndex(Cpu);

    status = EventChannelBindVirtualCpu(LocalPort, vcpu_id);
    if (!NT_SUCCESS(status))
        goto fail3;

    Channel->Cpu = Cpu;

    Info("[%u]: CPU %u\n", LocalPort, Cpu);

done:
    KeReleaseSpinLock(&Channel->Lock, Irql);

    return STATUS_SUCCESS;

fail3:
    Error("fail3\n");

    KeReleaseSpinLock(&Channel->Lock, Irql);

fail2:
    Error("fail2\n");

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static VOID
EvtchnUnmask(
    IN  PINTERFACE              Interface,
    IN  PXENBUS_EVTCHN_CHANNEL  Channel,
    IN  BOOLEAN                 InUpcall
    )
{
    PXENBUS_EVTCHN_CONTEXT      Context = Interface->Context;
    KIRQL                       Irql = PASSIVE_LEVEL;
    ULONG                       LocalPort;

    ASSERT3U(Channel->Magic, ==, XENBUS_EVTCHN_CHANNEL_MAGIC);

    if (!InUpcall)
        KeAcquireSpinLock(&Channel->Lock, &Irql);

    ASSERT3U(KeGetCurrentIrql(), >=, DISPATCH_LEVEL);

    if (!Channel->Active)
        goto done;

    LocalPort = Channel->LocalPort;

    if (XENBUS_EVTCHN_ABI(PortUnmask,
                          &Context->EvtchnAbi,
                          LocalPort))
        (VOID) EventChannelUnmask(LocalPort);

done:
    if (!InUpcall)
        KeReleaseSpinLock(&Channel->Lock, Irql);
}

static BOOLEAN
EvtchnUnmaskVersion1(
    IN  PINTERFACE              Interface,
    IN  PXENBUS_EVTCHN_CHANNEL  Channel,
    IN  BOOLEAN                 InUpcall
    )
{
    EvtchnUnmask(Interface, Channel, InUpcall);

    return FALSE;
}

static NTSTATUS
EvtchnSend(
    IN  PINTERFACE              Interface,
    IN  PXENBUS_EVTCHN_CHANNEL  Channel
    )
{
    KIRQL                       Irql;
    NTSTATUS                    status;

    UNREFERENCED_PARAMETER(Interface);

    ASSERT3U(Channel->Magic, ==, XENBUS_EVTCHN_CHANNEL_MAGIC);

    // Make sure we don't suspend
    KeRaiseIrql(DISPATCH_LEVEL, &Irql);

    status = STATUS_UNSUCCESSFUL;
    if (!Channel->Active)
        goto done;

    status = EventChannelSend(Channel->LocalPort);

done:
    KeLowerIrql(Irql);

    return status;
}

static VOID
EvtchnClose(
    IN  PINTERFACE              Interface,
    IN  PXENBUS_EVTCHN_CHANNEL  Channel
    )
{
    PXENBUS_EVTCHN_CONTEXT      Context = Interface->Context;
    ULONG                       LocalPort = Channel->LocalPort;
    KIRQL                       Irql;

    ASSERT3U(Channel->Magic, ==, XENBUS_EVTCHN_CHANNEL_MAGIC);

    KeRaiseIrql(DISPATCH_LEVEL, &Irql); // Prevent suspend

    Trace("%u\n", LocalPort);

    if (Channel->Active) {
        NTSTATUS    status;

        Channel->Active = FALSE;

        XENBUS_EVTCHN_ABI(PortDisable,
                          &Context->EvtchnAbi,
                          LocalPort);

        status = HashTableRemove(Context->Table, LocalPort);
        ASSERT(NT_SUCCESS(status));

        //
        // The event may be pending on a CPU queue so we mark it as
        // closed but defer the rest of the work to the correct
        // DPC, which will make sure the queue is polled first.
        //

        Channel->Closed = TRUE;
        KeMemoryBarrier();

        EvtchnTrigger(Interface, Channel);
        goto done;
    }

    KeAcquireSpinLockAtDpcLevel(&Context->Lock);

    Channel->Closed = TRUE;
    EvtchnReap(Context, Channel, FALSE);

    KeReleaseSpinLockFromDpcLevel(&Context->Lock);

done:
    KeLowerIrql(Irql);
}

static ULONG
EvtchnGetPort(
    IN  PINTERFACE              Interface,
    IN  PXENBUS_EVTCHN_CHANNEL  Channel
    )
{
    UNREFERENCED_PARAMETER(Interface);

    ASSERT3U(Channel->Magic, ==, XENBUS_EVTCHN_CHANNEL_MAGIC);
    ASSERT(Channel->Active);

    return Channel->LocalPort;
}

static
_Function_class_(KSERVICE_ROUTINE)
__drv_requiresIRQL(HIGH_LEVEL)
BOOLEAN
EvtchnInterruptCallback(
    IN  PKINTERRUPT         InterruptObject,
    IN  PVOID               Argument
    )
{
    PXENBUS_EVTCHN_CONTEXT  Context = Argument;
    ULONG                   Cpu;
    BOOLEAN                 DoneSomething;

    UNREFERENCED_PARAMETER(InterruptObject);

    ASSERT3U(KeGetCurrentIrql(), >=, DISPATCH_LEVEL);
    Cpu = KeGetCurrentProcessorNumber();

    DoneSomething = FALSE;
    while (XENBUS_SHARED_INFO(UpcallPending,
                              &Context->SharedInfoInterface,
                              Cpu))
        DoneSomething |= EvtchnPoll(Context, Cpu, NULL);

    return DoneSomething;
}

static NTSTATUS
EvtchnAbiAcquire(
    IN  PXENBUS_EVTCHN_CONTEXT  Context
    )
{
    NTSTATUS                    status;

    if (Context->UseEvtchnFifoAbi) {
        EvtchnFifoGetAbi(Context->EvtchnFifoContext,
                         &Context->EvtchnAbi);

        status = XENBUS_EVTCHN_ABI(Acquire, &Context->EvtchnAbi);
        if (!NT_SUCCESS(status))
            goto use_two_level;

        Info("FIFO\n");
        goto done;
    }

use_two_level:
    EvtchnTwoLevelGetAbi(Context->EvtchnTwoLevelContext,
                         &Context->EvtchnAbi);

    status = XENBUS_EVTCHN_ABI(Acquire, &Context->EvtchnAbi);
    if (!NT_SUCCESS(status))
        goto fail1;

    Info("TWO LEVEL\n");

done:
    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static VOID
EvtchnAbiRelease(
    IN  PXENBUS_EVTCHN_CONTEXT  Context
    )
{
    XENBUS_EVTCHN_ABI(Release, &Context->EvtchnAbi);

    RtlZeroMemory(&Context->EvtchnAbi, sizeof (XENBUS_EVTCHN_ABI));
}

static VOID
EvtchnInterruptEnable(
    IN  PXENBUS_EVTCHN_CONTEXT  Context
    )
{
    LONG                        Cpu;
    ULONG                       Line;
    NTSTATUS                    status;

    Trace("====>\n");

    ASSERT3U(Context->Affinity, ==, 0);

    Cpu = 0;
    while (Cpu < KeNumberProcessors) {
        unsigned int    vcpu_id;
        UCHAR           Vector;

        vcpu_id = SystemVirtualCpuIndex(Cpu);
        Vector = FdoGetInterruptVector(Context->Fdo,
                                       Context->LatchedInterrupt[Cpu]);

        status = HvmSetEvtchnUpcallVector(vcpu_id, Vector);
        if (NT_SUCCESS(status)) {
            Info("CPU %u\n", Cpu);
            Context->Affinity |= (KAFFINITY)1 << Cpu;
        }

        Cpu++;
    }

    Line = FdoGetInterruptLine(Context->Fdo,
                               Context->LevelSensitiveInterrupt);

    status = HvmSetParam(HVM_PARAM_CALLBACK_IRQ, Line);
    ASSERT(NT_SUCCESS(status));

    Trace("<====\n");
}

static VOID
EvtchnInterruptDisable(
    IN  PXENBUS_EVTCHN_CONTEXT  Context
    )
{
    LONG                        Cpu;
    NTSTATUS                    status;

    UNREFERENCED_PARAMETER(Context);

    Trace("====>\n");

    status = HvmSetParam(HVM_PARAM_CALLBACK_IRQ, 0);
    ASSERT(NT_SUCCESS(status));

    Cpu = KeNumberProcessors;
    while (--Cpu >= 0) {
        unsigned int    vcpu_id;

        vcpu_id = SystemVirtualCpuIndex(Cpu);

        (VOID) HvmSetEvtchnUpcallVector(vcpu_id, 0);
        Context->Affinity &= ~((KAFFINITY)1 << Cpu);
    }

    ASSERT3U(Context->Affinity, ==, 0);

    Trace("<====\n");
}

static VOID
EvtchnSuspendCallbackEarly(
    IN  PVOID               Argument
    )
{
    PXENBUS_EVTCHN_CONTEXT  Context = Argument;
    PLIST_ENTRY             ListEntry;

    for (ListEntry = Context->List.Flink;
         ListEntry != &Context->List;
         ListEntry = ListEntry->Flink) {
        PXENBUS_EVTCHN_CHANNEL  Channel;

        Channel = CONTAINING_RECORD(ListEntry, XENBUS_EVTCHN_CHANNEL, ListEntry);

        ASSERT3U(Channel->Magic, ==, XENBUS_EVTCHN_CHANNEL_MAGIC);

        if (Channel->Active) {
            ULONG       LocalPort = Channel->LocalPort;
            NTSTATUS    status;

            Channel->Active = FALSE;

            status = HashTableRemove(Context->Table, LocalPort);
            ASSERT(NT_SUCCESS(status));
        }
    }
}

static VOID
EvtchnSuspendCallbackLate(
    IN  PVOID               Argument
    )
{
    PXENBUS_EVTCHN_CONTEXT  Context = Argument;
    NTSTATUS                status;

    EvtchnAbiRelease(Context);

    status = EvtchnAbiAcquire(Context);
    ASSERT(NT_SUCCESS(status));

    EvtchnInterruptDisable(Context);
    EvtchnInterruptEnable(Context);
}

static VOID
EvtchnDebugCallback(
    IN  PVOID               Argument,
    IN  BOOLEAN             Crashing
    )
{
    PXENBUS_EVTCHN_CONTEXT  Context = Argument;

    UNREFERENCED_PARAMETER(Crashing);

    if (!IsListEmpty(&Context->List)) {
        PLIST_ENTRY ListEntry;

        XENBUS_DEBUG(Printf,
                     &Context->DebugInterface,
                     "EVENT CHANNELS:\n");

        for (ListEntry = Context->List.Flink;
             ListEntry != &Context->List;
             ListEntry = ListEntry->Flink) {
            PXENBUS_EVTCHN_CHANNEL  Channel;
            PCHAR                   Name;
            ULONG_PTR               Offset;

            Channel = CONTAINING_RECORD(ListEntry, XENBUS_EVTCHN_CHANNEL, ListEntry);

            ASSERT3U(Channel->Magic, ==, XENBUS_EVTCHN_CHANNEL_MAGIC);

            ModuleLookup((ULONG_PTR)Channel->Caller, &Name, &Offset);

            if (Name != NULL) {
                XENBUS_DEBUG(Printf,
                             &Context->DebugInterface,
                             "- (%04x) BY %s + %p %s%s\n",
                             Channel->LocalPort,
                             Name,
                             (PVOID)Offset,
                             (Channel->Mask) ? "AUTO-MASK " : "",
                             (Channel->Active) ? "ACTIVE" : "");
            } else {
                XENBUS_DEBUG(Printf,
                             &Context->DebugInterface,
                             "- (%04x) BY %p %s%s\n",
                             Channel->LocalPort,
                             (PVOID)Channel->Caller,
                             (Channel->Mask) ? "AUTO-MASK " : "",
                             (Channel->Active) ? "ACTIVE" : "");
            }

            switch (Channel->Type) {
            case XENBUS_EVTCHN_TYPE_FIXED:
                XENBUS_DEBUG(Printf,
                             &Context->DebugInterface,
                             "FIXED\n");
                break;

            case XENBUS_EVTCHN_TYPE_UNBOUND:
                XENBUS_DEBUG(Printf,
                             &Context->DebugInterface,
                             "UNBOUND: RemoteDomain = %u\n",
                             Channel->Parameters.Unbound.RemoteDomain);
                break;

            case XENBUS_EVTCHN_TYPE_INTER_DOMAIN:
                XENBUS_DEBUG(Printf,
                             &Context->DebugInterface,
                             "INTER_DOMAIN: RemoteDomain = %u RemotePort = %u\n",
                             Channel->Parameters.InterDomain.RemoteDomain,
                             Channel->Parameters.InterDomain.RemotePort);
                break;

            case XENBUS_EVTCHN_TYPE_VIRQ:
                XENBUS_DEBUG(Printf,
                             &Context->DebugInterface,
                             "VIRQ: Index = %u\n",
                             Channel->Parameters.Virq.Index);
                break;

            default:
                break;
            }
        }
    }
}

static NTSTATUS
EvtchnAcquire(
    IN  PINTERFACE          Interface
    )
{
    PXENBUS_EVTCHN_CONTEXT  Context = Interface->Context;
    PXENBUS_FDO             Fdo = Context->Fdo;
    KIRQL                   Irql;
    LONG                    Cpu;
    NTSTATUS                status;

    KeAcquireSpinLock(&Context->Lock, &Irql);

    if (Context->References++ != 0)
        goto done;

    Trace("====>\n");

    status = XENBUS_SUSPEND(Acquire, &Context->SuspendInterface);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = XENBUS_SUSPEND(Register,
                            &Context->SuspendInterface,
                            SUSPEND_CALLBACK_EARLY,
                            EvtchnSuspendCallbackEarly,
                            Context,
                            &Context->SuspendCallbackEarly);
    if (!NT_SUCCESS(status))
        goto fail2;

    status = XENBUS_SUSPEND(Register,
                            &Context->SuspendInterface,
                            SUSPEND_CALLBACK_LATE,
                            EvtchnSuspendCallbackLate,
                            Context,
                            &Context->SuspendCallbackLate);
    if (!NT_SUCCESS(status))
        goto fail3;

    status = XENBUS_DEBUG(Acquire, &Context->DebugInterface);
    if (!NT_SUCCESS(status))
        goto fail4;

    status = XENBUS_DEBUG(Register,
                          &Context->DebugInterface,
                          __MODULE__ "|EVTCHN",
                          EvtchnDebugCallback,
                          Context,
                          &Context->DebugCallback);
    if (!NT_SUCCESS(status))
        goto fail5;

    status = XENBUS_SHARED_INFO(Acquire, &Context->SharedInfoInterface);
    if (!NT_SUCCESS(status))
        goto fail6;

    status = EvtchnAbiAcquire(Context);
    if (!NT_SUCCESS(status))
        goto fail7;

    status = FdoAllocateInterrupt(Fdo,
                                  LevelSensitive,
                                  0,
                                  EvtchnInterruptCallback,
                                  Context,
                                  &Context->LevelSensitiveInterrupt);
    if (!NT_SUCCESS(status))
        goto fail8;

    Cpu = 0;
    while (Cpu < KeNumberProcessors) {
        status = FdoAllocateInterrupt(Fdo,
                                      Latched,
                                      Cpu,
                                      EvtchnInterruptCallback,
                                      Context,
                                      &Context->LatchedInterrupt[Cpu]);
        if (!NT_SUCCESS(status))
            goto fail9;

        Cpu++;
    }

    EvtchnInterruptEnable(Context);

    Trace("<====\n");

done:
    KeReleaseSpinLock(&Context->Lock, Irql);

    return STATUS_SUCCESS;

fail9:
    Error("fail9\n");

    while (--Cpu >= 0) {
        FdoFreeInterrupt(Fdo, Context->LatchedInterrupt[Cpu]);
        Context->LatchedInterrupt[Cpu] = NULL;
    }

    FdoFreeInterrupt(Fdo, Context->LevelSensitiveInterrupt);
    Context->LevelSensitiveInterrupt = NULL;

fail8:
    Error("fail8\n");

    EvtchnAbiRelease(Context);

fail7:
    Error("fail7\n");

    XENBUS_SHARED_INFO(Release, &Context->SharedInfoInterface);

fail6:
    Error("fail6\n");

    XENBUS_DEBUG(Deregister,
                 &Context->DebugInterface,
                 Context->DebugCallback);
    Context->DebugCallback = NULL;

fail5:
    Error("fail5\n");

    XENBUS_DEBUG(Release, &Context->DebugInterface);

fail4:
    Error("fail4\n");

    XENBUS_SUSPEND(Deregister,
                   &Context->SuspendInterface,
                   Context->SuspendCallbackLate);
    Context->SuspendCallbackLate = NULL;

fail3:
    Error("fail3\n");

    XENBUS_SUSPEND(Deregister,
                   &Context->SuspendInterface,
                   Context->SuspendCallbackEarly);
    Context->SuspendCallbackEarly = NULL;

fail2:
    Error("fail2\n");

    XENBUS_SUSPEND(Release, &Context->SuspendInterface);

fail1:
    Error("fail1 (%08x)\n", status);

    --Context->References;
    ASSERT3U(Context->References, ==, 0);
    KeReleaseSpinLock(&Context->Lock, Irql);

    return status;
}

VOID
EvtchnRelease(
    IN  PINTERFACE          Interface
    )
{
    PXENBUS_EVTCHN_CONTEXT  Context = Interface->Context;
    PXENBUS_FDO             Fdo = Context->Fdo;
    KIRQL                   Irql;
    LONG                    Cpu;

    KeAcquireSpinLock(&Context->Lock, &Irql);

    if (--Context->References > 0)
        goto done;

    Trace("====>\n");

    EvtchnInterruptDisable(Context);

    Cpu = KeNumberProcessors;
    while (--Cpu >= 0) {
        EvtchnFlush(Context, Cpu);

        FdoFreeInterrupt(Fdo, Context->LatchedInterrupt[Cpu]);
        Context->LatchedInterrupt[Cpu] = NULL;
    }

    FdoFreeInterrupt(Fdo, Context->LevelSensitiveInterrupt);
    Context->LevelSensitiveInterrupt = NULL;

    if (!IsListEmpty(&Context->List))
        BUG("OUTSTANDING EVENT CHANNELS");

    EvtchnAbiRelease(Context);

    XENBUS_SHARED_INFO(Release, &Context->SharedInfoInterface);

    XENBUS_DEBUG(Deregister,
                 &Context->DebugInterface,
                 Context->DebugCallback);
    Context->DebugCallback = NULL;

    XENBUS_DEBUG(Release, &Context->DebugInterface);

    XENBUS_SUSPEND(Deregister,
                   &Context->SuspendInterface,
                   Context->SuspendCallbackLate);
    Context->SuspendCallbackLate = NULL;

    XENBUS_SUSPEND(Deregister,
                   &Context->SuspendInterface,
                   Context->SuspendCallbackEarly);
    Context->SuspendCallbackEarly = NULL;

    XENBUS_SUSPEND(Release, &Context->SuspendInterface);

    Trace("<====\n");

done:
    KeReleaseSpinLock(&Context->Lock, Irql);
}

static struct _XENBUS_EVTCHN_INTERFACE_V1 EvtchnInterfaceVersion1 = {
    { sizeof (struct _XENBUS_EVTCHN_INTERFACE_V1), 1, NULL, NULL, NULL },
    EvtchnAcquire,
    EvtchnRelease,
    EvtchnOpen,
    EvtchnUnmaskVersion1,
    EvtchnSend,
    EvtchnTrigger,
    EvtchnGetPort,
    EvtchnClose
};
                     
static struct _XENBUS_EVTCHN_INTERFACE_V2 EvtchnInterfaceVersion2 = {
    { sizeof (struct _XENBUS_EVTCHN_INTERFACE_V2), 2, NULL, NULL, NULL },
    EvtchnAcquire,
    EvtchnRelease,
    EvtchnOpen,
    EvtchnBind,
    EvtchnUnmaskVersion1,
    EvtchnSend,
    EvtchnTrigger,
    EvtchnGetPort,
    EvtchnClose
};

static struct _XENBUS_EVTCHN_INTERFACE_V3 EvtchnInterfaceVersion3 = {
    { sizeof (struct _XENBUS_EVTCHN_INTERFACE_V3), 3, NULL, NULL, NULL },
    EvtchnAcquire,
    EvtchnRelease,
    EvtchnOpen,
    EvtchnBind,
    EvtchnUnmask,
    EvtchnSend,
    EvtchnTrigger,
    EvtchnGetPort,
    EvtchnClose
};

NTSTATUS
EvtchnInitialize(
    IN  PXENBUS_FDO             Fdo,
    OUT PXENBUS_EVTCHN_CONTEXT  *Context
    )
{
    HANDLE                      ParametersKey;
    ULONG                       UseEvtchnFifoAbi;
    ULONG                       Cpu;
    NTSTATUS                    status;

    Trace("====>\n");

    *Context = __EvtchnAllocate(sizeof (XENBUS_EVTCHN_CONTEXT));

    status = STATUS_NO_MEMORY;
    if (*Context == NULL)
        goto fail1;

    status = HashTableCreate(&(*Context)->Table);
    if (!NT_SUCCESS(status))
        goto fail2;

    status = EvtchnTwoLevelInitialize(Fdo,
                                      &(*Context)->EvtchnTwoLevelContext);
    if (!NT_SUCCESS(status))
        goto fail3;

    status = EvtchnFifoInitialize(Fdo, &(*Context)->EvtchnFifoContext);
    if (!NT_SUCCESS(status))
        goto fail4;

    ParametersKey = DriverGetParametersKey();

    status = RegistryQueryDwordValue(ParametersKey,
                                     "UseEvtchnFifoAbi",
                                     &UseEvtchnFifoAbi);
    if (!NT_SUCCESS(status))
        UseEvtchnFifoAbi = 1;

    (*Context)->UseEvtchnFifoAbi = (UseEvtchnFifoAbi != 0) ? TRUE : FALSE;

    status = SuspendGetInterface(FdoGetSuspendContext(Fdo),
                                 XENBUS_SUSPEND_INTERFACE_VERSION_MAX,
                                 (PINTERFACE)&(*Context)->SuspendInterface,
                                 sizeof ((*Context)->SuspendInterface));
    ASSERT(NT_SUCCESS(status));
    ASSERT((*Context)->SuspendInterface.Interface.Context != NULL);

    status = DebugGetInterface(FdoGetDebugContext(Fdo),
                               XENBUS_DEBUG_INTERFACE_VERSION_MAX,
                               (PINTERFACE)&(*Context)->DebugInterface,
                               sizeof ((*Context)->DebugInterface));
    ASSERT(NT_SUCCESS(status));
    ASSERT((*Context)->DebugInterface.Interface.Context != NULL);

    status = SharedInfoGetInterface(FdoGetSharedInfoContext(Fdo),
                                    XENBUS_SHARED_INFO_INTERFACE_VERSION_MAX,
                                    (PINTERFACE)&(*Context)->SharedInfoInterface,
                                    sizeof ((*Context)->SharedInfoInterface));
    ASSERT(NT_SUCCESS(status));
    ASSERT((*Context)->SharedInfoInterface.Interface.Context != NULL);

    InitializeListHead(&(*Context)->List);
    KeInitializeSpinLock(&(*Context)->Lock);

    for (Cpu = 0; Cpu < MAXIMUM_PROCESSORS; Cpu++) {
        PKDPC   Dpc = &(*Context)->Dpc[Cpu];

        InitializeListHead(&(*Context)->PendingList[Cpu]);

        KeInitializeDpc(Dpc, EvtchnDpc, *Context);
        KeSetTargetProcessorDpc(Dpc, (CCHAR)Cpu);
    }

    (*Context)->Fdo = Fdo;

    Trace("<====\n");

    return STATUS_SUCCESS;

fail4:
    Error("fail4\n");

    EvtchnTwoLevelTeardown((*Context)->EvtchnTwoLevelContext);
    (*Context)->EvtchnTwoLevelContext = NULL;

fail3:
    Error("fail3\n");

    HashTableDestroy((*Context)->Table);
    (*Context)->Table = NULL;

fail2:
    Error("fail2\n");

    ASSERT(IsZeroMemory(*Context, sizeof (XENBUS_EVTCHN_CONTEXT)));
    __EvtchnFree(*Context);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

NTSTATUS
EvtchnGetInterface(
    IN      PXENBUS_EVTCHN_CONTEXT  Context,
    IN      ULONG                   Version,
    IN OUT  PINTERFACE              Interface,
    IN      ULONG                   Size
    )
{
    NTSTATUS                        status;

    ASSERT(Context != NULL);

    switch (Version) {
    case 1: {
        struct _XENBUS_EVTCHN_INTERFACE_V1  *EvtchnInterface;

        EvtchnInterface = (struct _XENBUS_EVTCHN_INTERFACE_V1 *)Interface;

        status = STATUS_BUFFER_OVERFLOW;
        if (Size < sizeof (struct _XENBUS_EVTCHN_INTERFACE_V1))
            break;

        *EvtchnInterface = EvtchnInterfaceVersion1;

        ASSERT3U(Interface->Version, ==, Version);
        Interface->Context = Context;

        status = STATUS_SUCCESS;
        break;
    }
    case 2: {
        struct _XENBUS_EVTCHN_INTERFACE_V2  *EvtchnInterface;

        EvtchnInterface = (struct _XENBUS_EVTCHN_INTERFACE_V2 *)Interface;

        status = STATUS_BUFFER_OVERFLOW;
        if (Size < sizeof (struct _XENBUS_EVTCHN_INTERFACE_V2))
            break;

        *EvtchnInterface = EvtchnInterfaceVersion2;

        ASSERT3U(Interface->Version, ==, Version);
        Interface->Context = Context;

        status = STATUS_SUCCESS;
        break;
    }
    case 3: {
        struct _XENBUS_EVTCHN_INTERFACE_V3  *EvtchnInterface;

        EvtchnInterface = (struct _XENBUS_EVTCHN_INTERFACE_V3 *)Interface;

        status = STATUS_BUFFER_OVERFLOW;
        if (Size < sizeof (struct _XENBUS_EVTCHN_INTERFACE_V3))
            break;

        *EvtchnInterface = EvtchnInterfaceVersion3;

        ASSERT3U(Interface->Version, ==, Version);
        Interface->Context = Context;

        status = STATUS_SUCCESS;
        break;
    }
    default:
        status = STATUS_NOT_SUPPORTED;
        break;
    }

    return status;
}   

VOID
EvtchnTeardown(
    IN  PXENBUS_EVTCHN_CONTEXT  Context
    )
{
    Trace("====>\n");

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);
    KeFlushQueuedDpcs();

    Context->Fdo = NULL;

    RtlZeroMemory(&Context->Dpc, sizeof (KDPC) * MAXIMUM_PROCESSORS);
    RtlZeroMemory(&Context->Lock, sizeof (KSPIN_LOCK));
    RtlZeroMemory(&Context->List, sizeof (LIST_ENTRY));
    RtlZeroMemory(&Context->PendingList, sizeof (LIST_ENTRY) * MAXIMUM_PROCESSORS);

    RtlZeroMemory(&Context->SharedInfoInterface,
                  sizeof (XENBUS_SHARED_INFO_INTERFACE));

    RtlZeroMemory(&Context->DebugInterface,
                  sizeof (XENBUS_DEBUG_INTERFACE));

    RtlZeroMemory(&Context->SuspendInterface,
                  sizeof (XENBUS_SUSPEND_INTERFACE));

    Context->UseEvtchnFifoAbi = FALSE;

    EvtchnFifoTeardown(Context->EvtchnFifoContext);
    Context->EvtchnFifoContext = NULL;

    EvtchnTwoLevelTeardown(Context->EvtchnTwoLevelContext);
    Context->EvtchnTwoLevelContext = NULL;

    HashTableDestroy(Context->Table);
    Context->Table = NULL;

    ASSERT(IsZeroMemory(Context, sizeof (XENBUS_EVTCHN_CONTEXT)));
    __EvtchnFree(Context);

    Trace("<====\n");
}
