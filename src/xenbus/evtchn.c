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
#include <procgrp.h>
#include <stdarg.h>
#include <xen.h>

#include "evtchn.h"
#include "evtchn_2l.h"
#include "evtchn_fifo.h"
#include "fdo.h"
#include "hash_table.h"
#include "registry.h"
#include "dbg_print.h"
#include "assert.h"
#include "util.h"

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
    LONG                        Pending;
    PVOID                       Caller;
    PKSERVICE_ROUTINE           Callback;
    PVOID                       Argument;
    BOOLEAN                     Active; // Must be tested at >= DISPATCH_LEVEL
    ULONG                       Count;
    XENBUS_EVTCHN_TYPE          Type;
    XENBUS_EVTCHN_PARAMETERS    Parameters;
    BOOLEAN                     Mask;
    ULONG                       LocalPort;
    ULONG                       Cpu;
    BOOLEAN                     Closed;
};

typedef struct _XENBUS_EVTCHN_PROCESSOR {
    PXENBUS_EVTCHN_CONTEXT  Context;
    ULONG                   Cpu;
    PXENBUS_INTERRUPT       Interrupt;
    LIST_ENTRY              PendingList;
    KDPC                    Dpc;
    BOOLEAN                 UpcallEnabled;
} XENBUS_EVTCHN_PROCESSOR, *PXENBUS_EVTCHN_PROCESSOR;

struct _XENBUS_EVTCHN_CONTEXT {
    PXENBUS_FDO                     Fdo;
    KSPIN_LOCK                      Lock;
    LONG                            References;
    PXENBUS_INTERRUPT               Interrupt;
    PXENBUS_EVTCHN_PROCESSOR        Processor;
    ULONG                           ProcessorCount;
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
    BOOLEAN                         UseEvtchnUpcall;
    PXENBUS_HASH_TABLE              Table;
    LIST_ENTRY                      List;
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
    __FreePoolWithTag(Buffer, XENBUS_EVTCHN_TAG);
}

static NTSTATUS
EvtchnOpenFixed(
    IN  PXENBUS_EVTCHN_CONTEXT  Context,
    IN  PXENBUS_EVTCHN_CHANNEL  Channel,
    IN  ULONG                   Count,
    IN  va_list                 Arguments
    )
{
    ULONG                       LocalPort;
    BOOLEAN                     Mask;

    UNREFERENCED_PARAMETER(Context);

    ASSERT3U(Count, ==, 2);
    LocalPort = va_arg(Arguments, ULONG);
    Mask = va_arg(Arguments, BOOLEAN);

    Channel->Mask = Mask;
    Channel->LocalPort = LocalPort;

    return STATUS_SUCCESS;
}

static NTSTATUS
EvtchnOpenUnbound(
    IN  PXENBUS_EVTCHN_CONTEXT  Context,
    IN  PXENBUS_EVTCHN_CHANNEL  Channel,
    IN  ULONG                   Count,
    IN  va_list                 Arguments
    )
{
    USHORT                      RemoteDomain;
    BOOLEAN                     Mask;
    ULONG                       LocalPort;
    NTSTATUS                    status;

    UNREFERENCED_PARAMETER(Context);

    ASSERT3U(Count, ==, 2);
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
    IN  PXENBUS_EVTCHN_CONTEXT  Context,
    IN  PXENBUS_EVTCHN_CHANNEL  Channel,
    IN  ULONG                   Count,
    IN  va_list                 Arguments
    )
{
    USHORT                      RemoteDomain;
    ULONG                       RemotePort;
    BOOLEAN                     Mask;
    ULONG                       LocalPort;
    NTSTATUS                    status;

    UNREFERENCED_PARAMETER(Context);

    ASSERT3U(Count, ==, 3);
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
    IN  PXENBUS_EVTCHN_CONTEXT  Context,
    IN  PXENBUS_EVTCHN_CHANNEL  Channel,
    IN  ULONG                   Count,
    IN  va_list                 Arguments
    )
{
    ULONG                       Index;
    USHORT                      Group;
    UCHAR                       Number;
    PROCESSOR_NUMBER            ProcNumber;
    ULONG                       Cpu;
    PXENBUS_EVTCHN_PROCESSOR    Processor;
    unsigned int                vcpu_id;
    ULONG                       LocalPort;
    NTSTATUS                    status;

    Index = va_arg(Arguments, ULONG);

    if (Count == 1) {
        Group = 0;
        Number = 0;
    } else {
        ASSERT3U(Count, ==, 3);

        Group = va_arg(Arguments, USHORT);
        Number = va_arg(Arguments, UCHAR);
    }

    RtlZeroMemory(&ProcNumber, sizeof (PROCESSOR_NUMBER));
    ProcNumber.Group = Group;
    ProcNumber.Number = Number;

    Cpu = KeGetProcessorIndexFromNumber(&ProcNumber);

    ASSERT3U(Cpu, <, Context->ProcessorCount);
    Processor = &Context->Processor[Cpu];

    status = STATUS_NOT_SUPPORTED;
    if (!Processor->UpcallEnabled && Cpu != 0)
        goto fail1;

    status = SystemProcessorVcpuId(Cpu, &vcpu_id);
    ASSERT(NT_SUCCESS(status));

    status = EventChannelBindVirq(Index, vcpu_id, &LocalPort);
    if (!NT_SUCCESS(status))
        goto fail2;

    Channel->Parameters.Virq.Index = Index;

    Channel->LocalPort = LocalPort;

    return STATUS_SUCCESS;

fail2:
    Error("fail2\n");

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
        status = EvtchnOpenFixed(Context, Channel, 2, Arguments);
        break;

    case XENBUS_EVTCHN_TYPE_UNBOUND:
        status = EvtchnOpenUnbound(Context, Channel, 2, Arguments);
        break;

    case XENBUS_EVTCHN_TYPE_INTER_DOMAIN:
        status = EvtchnOpenInterDomain(Context, Channel, 3, Arguments);
        break;

    case XENBUS_EVTCHN_TYPE_VIRQ:
        // Processor information only specified from version 9 onwards
        status = EvtchnOpenVirq(Context, Channel,
                                (Interface->Version < 9) ? 1 : 3,
                                Arguments);
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

    Channel->Count = 0;

    ASSERT(Channel->Closed);
    Channel->Closed = FALSE;

    RtlZeroMemory(&Channel->Lock, sizeof (KSPIN_LOCK));

    RemoveEntryList(&Channel->ListEntry);
    RtlZeroMemory(&Channel->ListEntry, sizeof (LIST_ENTRY));

    Channel->Cpu = 0;

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
    IN  PVOID                   Argument,
    IN  ULONG                   LocalPort
    )
{
    PXENBUS_EVTCHN_PROCESSOR    Processor = Argument;
    PXENBUS_EVTCHN_CONTEXT      Context = Processor->Context;
    ULONG                       Cpu = Processor->Cpu;
    PXENBUS_EVTCHN_CHANNEL      Channel;
    NTSTATUS                    status;

    status = HashTableLookup(Context->Table,
                             LocalPort,
                             (PULONG_PTR)&Channel);
    if (!NT_SUCCESS(status))
        goto done;

    ASSERT3U(Channel->LocalPort, ==, LocalPort);

    if (Channel->Cpu != Cpu)
        goto done;

    if (InterlockedBitTestAndSet(&Channel->Pending, 0) == 0) {
        ASSERT(IsZeroMemory(&Channel->PendingListEntry, sizeof (LIST_ENTRY)));

        InsertTailList(&Processor->PendingList,
                       &Channel->PendingListEntry);
    }

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
    PXENBUS_EVTCHN_PROCESSOR    Processor;
    BOOLEAN                     DoneSomething;
    PLIST_ENTRY                 ListEntry;

    ASSERT3U(Cpu, <, Context->ProcessorCount);
    Processor = &Context->Processor[Cpu];

    (VOID) XENBUS_EVTCHN_ABI(Poll,
                             &Context->EvtchnAbi,
                             Cpu,
                             EvtchnPollCallback,
                             Processor);

    DoneSomething = FALSE;

    ListEntry = Processor->PendingList.Flink;
    while (ListEntry != &Processor->PendingList) {
        PLIST_ENTRY             Next = ListEntry->Flink;
        PXENBUS_EVTCHN_CHANNEL  Channel;

        Channel = CONTAINING_RECORD(ListEntry,
                                    XENBUS_EVTCHN_CHANNEL,
                                    PendingListEntry);

        ASSERT3U(Channel->Magic, ==, XENBUS_EVTCHN_CHANNEL_MAGIC);

        KeMemoryBarrier();
        if (!Channel->Closed) {
            ASSERT(Channel->Pending != 0);

            RemoveEntryList(&Channel->PendingListEntry);
            RtlZeroMemory(&Channel->PendingListEntry, sizeof (LIST_ENTRY));

            //
            // Make sure the list removal is complete before we allow the
            // channel to be queued again.
            //
            KeMemoryBarrier();

            Channel->Pending = 0;

            if (Channel->Mask)
                XENBUS_EVTCHN_ABI(PortMask,
                                  &Context->EvtchnAbi,
                                  Channel->LocalPort);

            XENBUS_EVTCHN_ABI(PortAck,
                              &Context->EvtchnAbi,
                              Channel->LocalPort);

	    /*
	     * Make sure the event is ack-ed before Count is incremented
	     * otherwise there is a small window where EvtchnWait() could
	     * end up waiting on the incremented value whilst new events
	     * would be missed (hence Count would not be further
	     * incremented to wake the waiter).
	     */
	    KeMemoryBarrier();
            Channel->Count++;

#pragma warning(suppress:6387)  // NULL argument
            DoneSomething |= Channel->Callback(NULL, Channel->Argument);
        } else if (List != NULL) {
            ASSERT(Channel->Pending != 0);

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
    PXENBUS_EVTCHN_PROCESSOR    Processor;
    LIST_ENTRY                  List;
    PXENBUS_INTERRUPT           Interrupt;
    KIRQL                       Irql;

    ASSERT3U(Cpu, <, Context->ProcessorCount);
    Processor = &Context->Processor[Cpu];

    Interrupt = (Processor->UpcallEnabled) ?
                Processor->Interrupt :
                Context->Interrupt;

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

        ASSERT(Channel->Pending != 0);

        RtlZeroMemory(&Channel->PendingListEntry, sizeof (LIST_ENTRY));

        // No need to barrier as the event will not be queued again
        Channel->Pending = 0;

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
    IN  PKDPC                   Dpc,
    IN  PVOID                   _Context,
    IN  PVOID                   Argument1,
    IN  PVOID                   Argument2
    )
{
    PXENBUS_EVTCHN_PROCESSOR    Processor = _Context;
    PXENBUS_EVTCHN_CONTEXT      Context = Processor->Context;
    ULONG                       Cpu = Processor->Cpu;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Argument1);
    UNREFERENCED_PARAMETER(Argument2);

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
    KIRQL                       Irql;
    ULONG                       Cpu;
    PXENBUS_EVTCHN_PROCESSOR    Processor;
    PXENBUS_INTERRUPT           Interrupt;
    BOOLEAN                     Queued;

    ASSERT3U(Channel->Magic, ==, XENBUS_EVTCHN_CHANNEL_MAGIC);

    KeAcquireSpinLock(&Channel->Lock, &Irql);
    Cpu = Channel->Cpu;
    KeReleaseSpinLock(&Channel->Lock, Irql);

    ASSERT3U(Cpu, <, Context->ProcessorCount);
    Processor = &Context->Processor[Cpu];

    Interrupt = (Processor->UpcallEnabled) ?
                Processor->Interrupt :
                Context->Interrupt;

    Irql = FdoAcquireInterruptLock(Context->Fdo, Interrupt);

    Queued = FALSE;
    if (InterlockedBitTestAndSet(&Channel->Pending, 0) == 0) {
        InsertTailList(&Processor->PendingList,
                       &Channel->PendingListEntry);
        Queued = TRUE;
    }

    FdoReleaseInterruptLock(Context->Fdo, Interrupt, Irql);

    if (!Queued)
        return;

    KeInsertQueueDpc(&Processor->Dpc, NULL, NULL);
}

static NTSTATUS
EvtchnBind(
    IN  PINTERFACE              Interface,
    IN  PXENBUS_EVTCHN_CHANNEL  Channel,
    IN  USHORT                  Group,
    IN  UCHAR                   Number
    )
{
    PXENBUS_EVTCHN_CONTEXT      Context = Interface->Context;
    PROCESSOR_NUMBER            ProcNumber;
    ULONG                       Cpu;
    PXENBUS_EVTCHN_PROCESSOR    Processor;
    ULONG                       LocalPort;
    unsigned int                vcpu_id;
    KIRQL                       Irql;
    NTSTATUS                    status;

    ASSERT3U(Channel->Magic, ==, XENBUS_EVTCHN_CHANNEL_MAGIC);

    RtlZeroMemory(&ProcNumber, sizeof (PROCESSOR_NUMBER));
    ProcNumber.Group = Group;
    ProcNumber.Number = Number;

    Cpu = KeGetProcessorIndexFromNumber(&ProcNumber);

    ASSERT3U(Cpu, <, Context->ProcessorCount);
    Processor = &Context->Processor[Cpu];

    status = STATUS_NOT_SUPPORTED;
    if (!Processor->UpcallEnabled && Cpu != 0)
        goto fail1;

    KeAcquireSpinLock(&Channel->Lock, &Irql);

    if (!Channel->Active)
        goto done;

    if (Channel->Cpu == Cpu)
        goto done;

    LocalPort = Channel->LocalPort;

    status = SystemProcessorVcpuId(Cpu, &vcpu_id);
    ASSERT(NT_SUCCESS(status));

    status = EventChannelBindVirtualCpu(LocalPort, vcpu_id);
    if (!NT_SUCCESS(status))
        goto fail2;

    Channel->Cpu = Cpu;

    Info("[%u]: CPU %u:%u\n", LocalPort, Group, Number);

done:
    KeReleaseSpinLock(&Channel->Lock, Irql);

    return STATUS_SUCCESS;

fail2:
    Error("fail2\n");

    KeReleaseSpinLock(&Channel->Lock, Irql);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static BOOLEAN
EvtchnUnmask(
    IN  PINTERFACE              Interface,
    IN  PXENBUS_EVTCHN_CHANNEL  Channel,
    IN  BOOLEAN                 InUpcall,
    IN  BOOLEAN                 Force
    )
{
    PXENBUS_EVTCHN_CONTEXT      Context = Interface->Context;
    KIRQL                       Irql = PASSIVE_LEVEL;
    BOOLEAN                     Pending;
    ULONG                       LocalPort;
    ULONG                       Cpu;

    ASSERT3U(Channel->Magic, ==, XENBUS_EVTCHN_CHANNEL_MAGIC);

    if (!InUpcall)
        KeAcquireSpinLock(&Channel->Lock, &Irql);

    ASSERT3U(KeGetCurrentIrql(), >=, DISPATCH_LEVEL);

    Pending = FALSE;

    if (!Channel->Active)
        goto done;

    LocalPort = Channel->LocalPort;

    Pending = XENBUS_EVTCHN_ABI(PortUnmask,
                                &Context->EvtchnAbi,
                                LocalPort);

    if (!Pending)
        goto done;

    //
    // If we are in context of the upcall, or we cannot tolerate a
    // failure to unmask, then use the hypercall.
    //
    if (InUpcall || Force) {
        XENBUS_EVTCHN_ABI(PortMask,
                          &Context->EvtchnAbi,
                          LocalPort);
        (VOID) EventChannelUnmask(LocalPort);

        Pending = FALSE;
        goto done;
    }

    //
    // If we are not unmasking on the same CPU to which the
    // event channel is bound, then we need to use the hypercall.
    // to schedule the upcall on the correct CPU.
    //
    Cpu = KeGetCurrentProcessorNumberEx(NULL);

    if (Channel->Cpu != Cpu) {
        XENBUS_EVTCHN_ABI(PortMask,
                          &Context->EvtchnAbi,
                          LocalPort);
        (VOID) EventChannelUnmask(LocalPort);

        Pending = FALSE;
        goto done;
    }

    if (Channel->Mask)
        XENBUS_EVTCHN_ABI(PortMask,
                          &Context->EvtchnAbi,
                          LocalPort);

    XENBUS_EVTCHN_ABI(PortAck,
                      &Context->EvtchnAbi,
                      LocalPort);

done:
    if (!InUpcall)
        KeReleaseSpinLock(&Channel->Lock, Irql);

    return Pending;
}

static VOID
EvtchnUnmaskVersion4(
    IN  PINTERFACE              Interface,
    IN  PXENBUS_EVTCHN_CHANNEL  Channel,
    IN  BOOLEAN                 InUpcall
    )
{
    EvtchnUnmask(Interface, Channel, InUpcall, TRUE);
}

static VOID
EvtchnSend(
    IN  PINTERFACE              Interface,
    IN  PXENBUS_EVTCHN_CHANNEL  Channel
    )
{
    UNREFERENCED_PARAMETER(Interface);

    ASSERT3U(Channel->Magic, ==, XENBUS_EVTCHN_CHANNEL_MAGIC);

    ASSERT3U(KeGetCurrentIrql(), >=, DISPATCH_LEVEL);

    if (Channel->Active)
        (VOID) EventChannelSend(Channel->LocalPort);
}

static VOID
EvtchnSendVersion1(
    IN  PINTERFACE              Interface,
    IN  PXENBUS_EVTCHN_CHANNEL  Channel
    )
{
    KIRQL                       Irql;

    KeRaiseIrql(DISPATCH_LEVEL, &Irql);
    EvtchnSend(Interface, Channel);
    KeLowerIrql(Irql);
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

static ULONG
EvtchnGetCount(
    IN  PINTERFACE              Interface,
    IN  PXENBUS_EVTCHN_CHANNEL  Channel
    )
{
    UNREFERENCED_PARAMETER(Interface);

    KeMemoryBarrier();

    return Channel->Count;
}

static NTSTATUS
EvtchnWait(
    IN  PINTERFACE              Interface,
    IN  PXENBUS_EVTCHN_CHANNEL  Channel,
    IN  ULONG                   Count,
    IN  PLARGE_INTEGER          Timeout
    )
{
    KIRQL                       Irql;
    LARGE_INTEGER               Start;
    NTSTATUS                    status;

    UNREFERENCED_PARAMETER(Interface);

    ASSERT3U(KeGetCurrentIrql(), <=, DISPATCH_LEVEL);
    KeRaiseIrql(DISPATCH_LEVEL, &Irql); // Prevent suspend

    KeQuerySystemTime(&Start);

    for (;;) {
        KeMemoryBarrier();

        status = STATUS_SUCCESS;
        if ((LONG64)Count - (LONG64)Channel->Count <= 0)
            break;

        if (Timeout != NULL) {
            LARGE_INTEGER   Now;

            KeQuerySystemTime(&Now);

            status = STATUS_TIMEOUT;
            if (Timeout->QuadPart > 0) {
                // Absolute timeout
                if (Now.QuadPart > Timeout->QuadPart)
                    break;
            } else if (Timeout->QuadPart < 0) {
                LONGLONG   Delta;

                // Relative timeout
                Delta = Now.QuadPart - Start.QuadPart;
                if (Delta > -Timeout->QuadPart)
                    break;
            } else {
                // Immediate timeout
                ASSERT(Timeout->QuadPart == 0);
                break;
            }
        }

        _mm_pause();
    }

    if (status == STATUS_TIMEOUT)
        Info("TIMED OUT: Count = %08x Channel->Count = %08x\n",
             Count,
             Channel->Count);

    KeLowerIrql(Irql);

    return status;
}

static NTSTATUS
EvtchnWaitVersion5(
    IN  PINTERFACE              Interface,
    IN  PXENBUS_EVTCHN_CHANNEL  Channel,
    IN  PLARGE_INTEGER          Timeout
    )
{
    ULONG                       Count;

    Count = EvtchnGetCount(Interface, Channel);

    return EvtchnWait(Interface,
                      Channel,
                      Count + 1,
                      Timeout);
}

static
_Function_class_(KSERVICE_ROUTINE)
__drv_requiresIRQL(HIGH_LEVEL)
BOOLEAN
EvtchnInterruptCallback(
    IN  PKINTERRUPT             InterruptObject,
    IN  PVOID                   Argument
    )
{
    PXENBUS_EVTCHN_PROCESSOR    Processor = Argument;
    PXENBUS_EVTCHN_CONTEXT      Context = Processor->Context;
    ULONG                       Cpu = Processor->Cpu;
    BOOLEAN                     DoneSomething;

    UNREFERENCED_PARAMETER(InterruptObject);

    DoneSomething = FALSE;
    while (XENBUS_SHARED_INFO(UpcallPending,
                              &Context->SharedInfoInterface,
                              Cpu))
        DoneSomething |= EvtchnPoll(Context, Cpu, NULL);

    return DoneSomething;
}

VOID
EvtchnReset(
    VOID
    )
{
    ULONGLONG                   Value;
    XENBUS_EVTCHN_CHANNEL       Store;
    XENBUS_EVTCHN_CHANNEL       Console;
    NTSTATUS                    status;

    //
    // When we reset the event channel ABI we will lose our
    // binding to the any event channel which was set up
    // by the toolstack during domain build.
    // We need to get the binding back, so we must query the
    // remote domain and port, and then re-bind after the
    // reset.
    //

    RtlZeroMemory(&Store, sizeof (Store));
    RtlZeroMemory(&Console, sizeof (Console));

    status = HvmGetParam(HVM_PARAM_STORE_EVTCHN, &Value);
    if (NT_SUCCESS(status))
        Store.LocalPort = (ULONG)Value;

    status = HvmGetParam(HVM_PARAM_CONSOLE_EVTCHN, &Value);
    if (NT_SUCCESS(status))
        Console.LocalPort = (ULONG)Value;

    if (Store.LocalPort != 0) {
        USHORT  RemoteDomain;
        ULONG   RemotePort;

        status = EventChannelQueryInterDomain(Store.LocalPort,
                                              &RemoteDomain,
                                              &RemotePort);
        ASSERT(NT_SUCCESS(status));

        Store.Parameters.InterDomain.RemoteDomain = RemoteDomain;
        Store.Parameters.InterDomain.RemotePort = RemotePort;

        LogPrintf(LOG_LEVEL_INFO, "EVTCHN_RESET: STORE (%u) -> (%u:%u)\n",
                  Store.LocalPort,
                  RemoteDomain,
                  RemotePort);
    }

    if (Console.LocalPort != 0) {
        USHORT  RemoteDomain;
        ULONG   RemotePort;

        status = EventChannelQueryInterDomain(Console.LocalPort,
                                              &RemoteDomain,
                                              &RemotePort);
        ASSERT(NT_SUCCESS(status));

        Console.Parameters.InterDomain.RemoteDomain = RemoteDomain;
        Console.Parameters.InterDomain.RemotePort = RemotePort;

        LogPrintf(LOG_LEVEL_INFO, "EVTCHN_RESET: CONSOLE (%u) -> (%u:%u)\n",
                  Console.LocalPort,
                  RemoteDomain,
                  RemotePort);
    }

    (VOID) EventChannelReset();
    LogPrintf(LOG_LEVEL_INFO, "EVTCHN_RESET: RESET\n");

    if (Store.LocalPort != 0) {
        USHORT  RemoteDomain;
        ULONG   RemotePort;

        RemoteDomain = Store.Parameters.InterDomain.RemoteDomain;
        RemotePort = Store.Parameters.InterDomain.RemotePort;

        status = EventChannelBindInterDomain(RemoteDomain,
                                             RemotePort,
                                             &Store.LocalPort);
        ASSERT(NT_SUCCESS(status));

        status = HvmSetParam(HVM_PARAM_STORE_EVTCHN, Store.LocalPort);
        ASSERT(NT_SUCCESS(status));

        LogPrintf(LOG_LEVEL_INFO, "EVTCHN_RESET: STORE (%u:%u) -> %u\n",
                  RemoteDomain,
                  RemotePort,
                  Store.LocalPort);
    }

    if (Console.LocalPort != 0) {
        USHORT  RemoteDomain;
        ULONG   RemotePort;

        RemoteDomain = Console.Parameters.InterDomain.RemoteDomain;
        RemotePort = Console.Parameters.InterDomain.RemotePort;

        status = EventChannelBindInterDomain(RemoteDomain,
                                             RemotePort,
                                             &Console.LocalPort);
        ASSERT(NT_SUCCESS(status));

        status = HvmSetParam(HVM_PARAM_CONSOLE_EVTCHN, Console.LocalPort);
        ASSERT(NT_SUCCESS(status));

        LogPrintf(LOG_LEVEL_INFO, "EVTCHN_RESET: CONSOLE (%u:%u) -> %u\n",
                  RemoteDomain,
                  RemotePort,
                  Console.LocalPort);
    }
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

        status = XENBUS_EVTCHN_ABI(Acquire,
                                   &Context->EvtchnAbi);
        if (!NT_SUCCESS(status))
            goto use_two_level;

        Info("FIFO\n");
        goto done;
    }

use_two_level:
    EvtchnTwoLevelGetAbi(Context->EvtchnTwoLevelContext,
                         &Context->EvtchnAbi);

    status = XENBUS_EVTCHN_ABI(Acquire,
                               &Context->EvtchnAbi);
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
    ULONG                       Cpu;
    ULONG                       Line;
    KIRQL                       Irql;
    NTSTATUS                    status;

    Trace("====>\n");

    if (!Context->UseEvtchnUpcall)
    {
        Info("PER-CPU UPCALL DISABLED\n");
        goto line;
    }

    for (Cpu = 0; Cpu < Context->ProcessorCount; Cpu++) {
        PXENBUS_EVTCHN_PROCESSOR    Processor;
        unsigned int                vcpu_id;
        UCHAR                       Vector;
        PROCESSOR_NUMBER            ProcNumber;

        Processor = &Context->Processor[Cpu];

        if (Processor->Interrupt == NULL)
            continue;

        status = SystemProcessorVcpuId(Cpu, &vcpu_id);
        ASSERT(NT_SUCCESS(status));

        Vector = FdoGetInterruptVector(Context->Fdo, Processor->Interrupt);

        status = HvmSetEvtchnUpcallVector(vcpu_id, Vector);
        if (!NT_SUCCESS(status)) {
            if (status != STATUS_NOT_IMPLEMENTED )
                continue;

            Info("PER-CPU UPCALL NOT IMPLEMENTED\n");
            break;
        }

        status = KeGetProcessorNumberFromIndex(Cpu, &ProcNumber);
        ASSERT(NT_SUCCESS(status));

        Info("CPU %u:%u (Vector = %u)\n",
             ProcNumber.Group,
             ProcNumber.Number,
             Vector);
        Processor->UpcallEnabled = TRUE;

        Irql = FdoAcquireInterruptLock(Context->Fdo, Processor->Interrupt);
        (VOID) EvtchnInterruptCallback(NULL, Processor);
        FdoReleaseInterruptLock(Context->Fdo, Processor->Interrupt, Irql);
    }

line:
    Line = FdoGetInterruptLine(Context->Fdo, Context->Interrupt);

    status = HvmSetParam(HVM_PARAM_CALLBACK_IRQ, Line);
    ASSERT(NT_SUCCESS(status));

    Info("CALLBACK VIA (Vector = %u)\n", Line);

    Irql = FdoAcquireInterruptLock(Context->Fdo, Context->Interrupt);
    (VOID) EvtchnInterruptCallback(NULL, &Context->Processor[0]);
    FdoReleaseInterruptLock(Context->Fdo, Context->Interrupt, Irql);

    Trace("<====\n");
}

static VOID
EvtchnInterruptDisable(
    IN  PXENBUS_EVTCHN_CONTEXT  Context
    )
{
    ULONG                       Cpu;
    NTSTATUS                    status;

    UNREFERENCED_PARAMETER(Context);

    Trace("====>\n");

    status = HvmSetParam(HVM_PARAM_CALLBACK_IRQ, 0);
    ASSERT(NT_SUCCESS(status));

    for (Cpu = 0; Cpu < Context->ProcessorCount; Cpu++) {
        PXENBUS_EVTCHN_PROCESSOR    Processor;
        unsigned int                vcpu_id;

        Processor = &Context->Processor[Cpu];

        if (!Processor->UpcallEnabled)
            continue;

        status = SystemProcessorVcpuId(Cpu, &vcpu_id);
        ASSERT(NT_SUCCESS(status));

        (VOID) HvmSetEvtchnUpcallVector(vcpu_id, 0);
        Processor->UpcallEnabled = FALSE;
    }

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

        ASSERT(ListEntry->Flink != NULL);

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

            XENBUS_DEBUG(Printf,
                         &Context->DebugInterface,
                         "Count = %lu\n",
                         Channel->Count);
        }
    }
}

static BOOLEAN
EvtchnIsProcessorEnabled(
    IN  PXENBUS_EVTCHN_CONTEXT      Context,
    IN  ULONG                       Cpu
    )
{
    vcpu_info_t                     *Vcpu;
    NTSTATUS                        status;

    status = SystemProcessorVcpuInfo(Cpu, &Vcpu);
    if (!NT_SUCCESS(status)) {
        unsigned int    vcpu_id;

        ASSERT(status == STATUS_NOT_SUPPORTED);

        status = SystemProcessorVcpuId(Cpu, &vcpu_id);
        ASSERT(NT_SUCCESS(status));

        if (vcpu_id >= XEN_LEGACY_MAX_VCPUS)
            return FALSE;
    }

    return XENBUS_EVTCHN_ABI(IsProcessorEnabled,
                             &Context->EvtchnAbi,
                             Cpu);
}

static NTSTATUS
EvtchnAcquire(
    IN  PINTERFACE          Interface
    )
{
    PXENBUS_EVTCHN_CONTEXT  Context = Interface->Context;
    PXENBUS_FDO             Fdo = Context->Fdo;
    KIRQL                   Irql;
    PROCESSOR_NUMBER        ProcNumber;
    ULONG                   Cpu;
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

    Context->ProcessorCount = KeQueryMaximumProcessorCountEx(ALL_PROCESSOR_GROUPS);
    Context->Processor = __EvtchnAllocate(sizeof (XENBUS_EVTCHN_PROCESSOR) * Context->ProcessorCount);

    status = STATUS_NO_MEMORY;
    if (Context->Processor == NULL)
        goto fail8;

    for (Cpu = 0; Cpu < Context->ProcessorCount; Cpu++) {
        PXENBUS_EVTCHN_PROCESSOR    Processor;

        if (!EvtchnIsProcessorEnabled(Context, Cpu))
            continue;

        status = KeGetProcessorNumberFromIndex(Cpu, &ProcNumber);
        ASSERT(NT_SUCCESS(status));

        Processor = &Context->Processor[Cpu];

        Processor->Context = Context;
        Processor->Cpu = Cpu;
        Processor->Interrupt = FdoAllocateInterrupt(Fdo,
                                                    Latched,
                                                    ProcNumber.Group,
                                                    ProcNumber.Number,
                                                    EvtchnInterruptCallback,
                                                    Processor);

        InitializeListHead(&Processor->PendingList);

        KeInitializeDpc(&Processor->Dpc, EvtchnDpc, Processor);
        KeSetTargetProcessorDpcEx(&Processor->Dpc, &ProcNumber);
    }

    status = KeGetProcessorNumberFromIndex(0, &ProcNumber);
    ASSERT(NT_SUCCESS(status));

    Context->Interrupt = FdoAllocateInterrupt(Fdo,
                                              LevelSensitive,
                                              ProcNumber.Group,
                                              ProcNumber.Number,
                                              EvtchnInterruptCallback,
                                              &Context->Processor[0]);

    status = STATUS_UNSUCCESSFUL;
    if (Context->Interrupt == NULL)
        goto fail9;

    EvtchnInterruptEnable(Context);

    Trace("<====\n");

done:
    KeReleaseSpinLock(&Context->Lock, Irql);

    return STATUS_SUCCESS;

fail9:
    Error("fail9\n");

    for (Cpu = 0; Cpu < Context->ProcessorCount; Cpu++) {
        PXENBUS_EVTCHN_PROCESSOR Processor;

        ASSERT(Context->Processor != NULL);
        Processor = &Context->Processor[Cpu];

        RtlZeroMemory(&Processor->Dpc, sizeof (KDPC));
        RtlZeroMemory(&Processor->PendingList, sizeof (LIST_ENTRY));

        if (Processor->Interrupt != NULL) {
            FdoFreeInterrupt(Fdo, Processor->Interrupt);
            Processor->Interrupt = NULL;
        }

        Processor->Cpu = 0;
        Processor->Context = NULL;
    }

    ASSERT(IsZeroMemory(Context->Processor, sizeof (XENBUS_EVTCHN_PROCESSOR) * Context->ProcessorCount));
    __EvtchnFree(Context->Processor);
    Context->Processor = NULL;

fail8:
    Error("fail8\n");

    Context->ProcessorCount = 0;
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
    ULONG                   Cpu;

    KeAcquireSpinLock(&Context->Lock, &Irql);

    if (--Context->References > 0)
        goto done;

    Trace("====>\n");

    EvtchnInterruptDisable(Context);

    for (Cpu = 0; Cpu < Context->ProcessorCount; Cpu++) {
        PXENBUS_EVTCHN_PROCESSOR Processor;

        ASSERT(Context->Processor != NULL);
        Processor = &Context->Processor[Cpu];

        EvtchnFlush(Context, Cpu);

        (VOID) KeRemoveQueueDpc(&Processor->Dpc);
        RtlZeroMemory(&Processor->Dpc, sizeof (KDPC));
        RtlZeroMemory(&Processor->PendingList, sizeof (LIST_ENTRY));

        if (Processor->Interrupt != NULL) {
            FdoFreeInterrupt(Fdo, Processor->Interrupt);
            Processor->Interrupt = NULL;
        }

        Processor->Cpu = 0;
        Processor->Context = NULL;
    }

    ASSERT(IsZeroMemory(Context->Processor, sizeof (XENBUS_EVTCHN_PROCESSOR) * Context->ProcessorCount));
    __EvtchnFree(Context->Processor);
    Context->Processor = NULL;
    Context->ProcessorCount = 0;

    FdoFreeInterrupt(Fdo, Context->Interrupt);
    Context->Interrupt = NULL;

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

static struct _XENBUS_EVTCHN_INTERFACE_V4 EvtchnInterfaceVersion4 = {
    { sizeof (struct _XENBUS_EVTCHN_INTERFACE_V4), 4, NULL, NULL, NULL },
    EvtchnAcquire,
    EvtchnRelease,
    EvtchnOpen,
    EvtchnBind,
    EvtchnUnmaskVersion4,
    EvtchnSendVersion1,
    EvtchnTrigger,
    EvtchnGetPort,
    EvtchnClose
};

static struct _XENBUS_EVTCHN_INTERFACE_V5 EvtchnInterfaceVersion5 = {
    { sizeof (struct _XENBUS_EVTCHN_INTERFACE_V5), 5, NULL, NULL, NULL },
    EvtchnAcquire,
    EvtchnRelease,
    EvtchnOpen,
    EvtchnBind,
    EvtchnUnmaskVersion4,
    EvtchnSendVersion1,
    EvtchnTrigger,
    EvtchnWaitVersion5,
    EvtchnGetPort,
    EvtchnClose,
};

static struct _XENBUS_EVTCHN_INTERFACE_V6 EvtchnInterfaceVersion6 = {
    { sizeof (struct _XENBUS_EVTCHN_INTERFACE_V6), 6, NULL, NULL, NULL },
    EvtchnAcquire,
    EvtchnRelease,
    EvtchnOpen,
    EvtchnBind,
    EvtchnUnmaskVersion4,
    EvtchnSend,
    EvtchnTrigger,
    EvtchnWaitVersion5,
    EvtchnGetPort,
    EvtchnClose,
};

static struct _XENBUS_EVTCHN_INTERFACE_V7 EvtchnInterfaceVersion7 = {
    { sizeof (struct _XENBUS_EVTCHN_INTERFACE_V7), 7, NULL, NULL, NULL },
    EvtchnAcquire,
    EvtchnRelease,
    EvtchnOpen,
    EvtchnBind,
    EvtchnUnmaskVersion4,
    EvtchnSend,
    EvtchnTrigger,
    EvtchnGetCount,
    EvtchnWait,
    EvtchnGetPort,
    EvtchnClose,
};

static struct _XENBUS_EVTCHN_INTERFACE_V8 EvtchnInterfaceVersion8 = {
    { sizeof (struct _XENBUS_EVTCHN_INTERFACE_V8), 8, NULL, NULL, NULL },
    EvtchnAcquire,
    EvtchnRelease,
    EvtchnOpen,
    EvtchnBind,
    EvtchnUnmask,
    EvtchnSend,
    EvtchnTrigger,
    EvtchnGetCount,
    EvtchnWait,
    EvtchnGetPort,
    EvtchnClose,
};

static struct _XENBUS_EVTCHN_INTERFACE_V9 EvtchnInterfaceVersion9 = {
    { sizeof (struct _XENBUS_EVTCHN_INTERFACE_V9), 9, NULL, NULL, NULL },
    EvtchnAcquire,
    EvtchnRelease,
    EvtchnOpen,
    EvtchnBind,
    EvtchnUnmask,
    EvtchnSend,
    EvtchnTrigger,
    EvtchnGetCount,
    EvtchnWait,
    EvtchnGetPort,
    EvtchnClose,
};

NTSTATUS
EvtchnInitialize(
    IN  PXENBUS_FDO             Fdo,
    OUT PXENBUS_EVTCHN_CONTEXT  *Context
    )
{
    HANDLE                      ParametersKey;
    ULONG                       UseEvtchnFifoAbi;
    ULONG                       UseEvtchnUpcall;
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

    status = RegistryQueryDwordValue(ParametersKey,
                                     "UseEvtchnUpcall",
                                     &UseEvtchnUpcall);
    if (!NT_SUCCESS(status))
        UseEvtchnUpcall = 1;

    (*Context)->UseEvtchnUpcall = (UseEvtchnUpcall != 0) ? TRUE : FALSE;

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
    case 4: {
        struct _XENBUS_EVTCHN_INTERFACE_V4  *EvtchnInterface;

        EvtchnInterface = (struct _XENBUS_EVTCHN_INTERFACE_V4 *)Interface;

        status = STATUS_BUFFER_OVERFLOW;
        if (Size < sizeof (struct _XENBUS_EVTCHN_INTERFACE_V4))
            break;

        *EvtchnInterface = EvtchnInterfaceVersion4;

        ASSERT3U(Interface->Version, ==, Version);
        Interface->Context = Context;

        status = STATUS_SUCCESS;
        break;
    }
    case 5: {
        struct _XENBUS_EVTCHN_INTERFACE_V5  *EvtchnInterface;

        EvtchnInterface = (struct _XENBUS_EVTCHN_INTERFACE_V5 *)Interface;

        status = STATUS_BUFFER_OVERFLOW;
        if (Size < sizeof (struct _XENBUS_EVTCHN_INTERFACE_V5))
            break;

        *EvtchnInterface = EvtchnInterfaceVersion5;

        ASSERT3U(Interface->Version, ==, Version);
        Interface->Context = Context;

        status = STATUS_SUCCESS;
        break;
    }
    case 6: {
        struct _XENBUS_EVTCHN_INTERFACE_V6  *EvtchnInterface;

        EvtchnInterface = (struct _XENBUS_EVTCHN_INTERFACE_V6 *)Interface;

        status = STATUS_BUFFER_OVERFLOW;
        if (Size < sizeof (struct _XENBUS_EVTCHN_INTERFACE_V6))
            break;

        *EvtchnInterface = EvtchnInterfaceVersion6;

        ASSERT3U(Interface->Version, ==, Version);
        Interface->Context = Context;

        status = STATUS_SUCCESS;
        break;
    }
    case 7: {
        struct _XENBUS_EVTCHN_INTERFACE_V7  *EvtchnInterface;

        EvtchnInterface = (struct _XENBUS_EVTCHN_INTERFACE_V7 *)Interface;

        status = STATUS_BUFFER_OVERFLOW;
        if (Size < sizeof (struct _XENBUS_EVTCHN_INTERFACE_V7))
            break;

        *EvtchnInterface = EvtchnInterfaceVersion7;

        ASSERT3U(Interface->Version, ==, Version);
        Interface->Context = Context;

        status = STATUS_SUCCESS;
        break;
    }
    case 8: {
        struct _XENBUS_EVTCHN_INTERFACE_V8  *EvtchnInterface;

        EvtchnInterface = (struct _XENBUS_EVTCHN_INTERFACE_V8 *)Interface;

        status = STATUS_BUFFER_OVERFLOW;
        if (Size < sizeof (struct _XENBUS_EVTCHN_INTERFACE_V8))
            break;

        *EvtchnInterface = EvtchnInterfaceVersion8;

        ASSERT3U(Interface->Version, ==, Version);
        Interface->Context = Context;

        status = STATUS_SUCCESS;
        break;
    }
    case 9: {
        struct _XENBUS_EVTCHN_INTERFACE_V9  *EvtchnInterface;

        EvtchnInterface = (struct _XENBUS_EVTCHN_INTERFACE_V9 *)Interface;

        status = STATUS_BUFFER_OVERFLOW;
        if (Size < sizeof (struct _XENBUS_EVTCHN_INTERFACE_V9))
            break;

        *EvtchnInterface = EvtchnInterfaceVersion9;

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

ULONG
EvtchnGetReferences(
    IN  PXENBUS_EVTCHN_CONTEXT  Context
    )
{
    return Context->References;
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

    RtlZeroMemory(&Context->Lock, sizeof (KSPIN_LOCK));
    RtlZeroMemory(&Context->List, sizeof (LIST_ENTRY));

    RtlZeroMemory(&Context->SharedInfoInterface,
                  sizeof (XENBUS_SHARED_INFO_INTERFACE));

    RtlZeroMemory(&Context->DebugInterface,
                  sizeof (XENBUS_DEBUG_INTERFACE));

    RtlZeroMemory(&Context->SuspendInterface,
                  sizeof (XENBUS_SUSPEND_INTERFACE));

    Context->UseEvtchnUpcall = FALSE;
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
