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
    LIST_ENTRY                  ListEntry;
    PVOID                       Caller;
    PKSERVICE_ROUTINE           Callback;
    PVOID                       Argument;
    BOOLEAN                     Active; // Must be tested at >= DISPATCH_LEVEL
    XENBUS_EVTCHN_TYPE          Type;
    XENBUS_EVTCHN_PARAMETERS    Parameters;
    BOOLEAN                     Mask;
    ULONG                       LocalPort;
};

struct _XENBUS_EVTCHN_CONTEXT {
    PXENBUS_FDO                     Fdo;
    KSPIN_LOCK                      Lock;
    LONG                            References;
    ULONG                           Vector;
    BOOLEAN                         Enabled;
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

static VOID
EvtchnInterruptEnable(
    IN  PXENBUS_EVTCHN_CONTEXT  Context
    )
{
    NTSTATUS                    status;

    Trace("<===>\n");

    status = HvmSetParam(HVM_PARAM_CALLBACK_IRQ, Context->Vector);
    ASSERT(NT_SUCCESS(status));
}

static VOID
EvtchnInterruptDisable(
    IN  PXENBUS_EVTCHN_CONTEXT  Context
    )
{
    NTSTATUS                    status;

    UNREFERENCED_PARAMETER(Context);

    Trace("<===>\n");

    status = HvmSetParam(HVM_PARAM_CALLBACK_IRQ, 0);
    ASSERT(NT_SUCCESS(status));
}

static FORCEINLINE
_IRQL_requires_max_(HIGH_LEVEL)
_IRQL_saves_
_IRQL_raises_(HIGH_LEVEL)
KIRQL
__EvtchnAcquireInterruptLock(
    IN  PXENBUS_EVTCHN_CONTEXT  Context
    )
{
    return FdoAcquireInterruptLock(Context->Fdo);
}

static FORCEINLINE
__drv_requiresIRQL(HIGH_LEVEL)
VOID
__EvtchnReleaseInterruptLock(
    IN  PXENBUS_EVTCHN_CONTEXT      Context,
    IN  __drv_restoresIRQL KIRQL    Irql
    )
{
    FdoReleaseInterruptLock(Context->Fdo, Irql);
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

    if (!IsListEmpty(&Context->List) && !Context->Enabled) {
        EvtchnInterruptEnable(Context);
        Context->Enabled = TRUE;
    }

    KeReleaseSpinLockFromDpcLevel(&Context->Lock);

    KeLowerIrql(Irql);

    return Channel;

fail4:
    Error("fail4\n");

    XENBUS_EVTCHN_ABI(PortDisable,
                      &Context->EvtchnAbi,
                      LocalPort);

fail3:
    Error("fail3\n");

    Channel->LocalPort = 0;
    RtlZeroMemory(&Channel->Parameters, sizeof (XENBUS_EVTCHN_PARAMETERS));

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

static BOOLEAN
EvtchnUnmask(
    IN  PINTERFACE              Interface,
    IN  PXENBUS_EVTCHN_CHANNEL  Channel,
    IN  BOOLEAN                 InCallback
    )
{
    PXENBUS_EVTCHN_CONTEXT      Context = Interface->Context;
    KIRQL                       Irql = PASSIVE_LEVEL;
    BOOLEAN                     Pending = FALSE;

    ASSERT3U(Channel->Magic, ==, XENBUS_EVTCHN_CHANNEL_MAGIC);

    if (!InCallback)
        KeRaiseIrql(DISPATCH_LEVEL, &Irql); // Prevent suspend

    ASSERT3U(KeGetCurrentIrql(), >=, DISPATCH_LEVEL);

    if (Channel->Active) {
        Pending = XENBUS_EVTCHN_ABI(PortUnmask,
                                    &Context->EvtchnAbi,
                                    Channel->LocalPort);

        if (Pending && Channel->Mask)
            XENBUS_EVTCHN_ABI(PortMask,
                              &Context->EvtchnAbi,
                              Channel->LocalPort);
    }

    if (!InCallback)
        KeLowerIrql(Irql);

    return Pending;
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

static BOOLEAN
EvtchnCallback(
    IN  PXENBUS_EVTCHN_CONTEXT  Context,
    IN  PXENBUS_EVTCHN_CHANNEL  Channel
    )
{
    BOOLEAN                     DoneSomething;

    UNREFERENCED_PARAMETER(Context);

    ASSERT(Channel != NULL);
    ASSERT(Channel->Active);

#pragma warning(suppress:6387)  // NULL argument
    DoneSomething = Channel->Callback(NULL, Channel->Argument);

    return DoneSomething;
}

static BOOLEAN
EvtchnTrigger(
    IN  PINTERFACE              Interface,
    IN  PXENBUS_EVTCHN_CHANNEL  Channel
    )
{
    PXENBUS_EVTCHN_CONTEXT      Context = Interface->Context;
    KIRQL                       Irql;
    BOOLEAN                     DoneSomething;

    ASSERT3U(Channel->Magic, ==, XENBUS_EVTCHN_CHANNEL_MAGIC);

    Irql = __EvtchnAcquireInterruptLock(Context);

    ASSERT3U(KeGetCurrentIrql(), >=, DISPATCH_LEVEL);

    if (Channel->Active) {
        DoneSomething = EvtchnCallback(Context, Channel);
    } else {
        Warning("[%d]: INVALID PORT\n", Channel->LocalPort);
        DoneSomething = FALSE;
    }

    __EvtchnReleaseInterruptLock(Context, Irql);

    return DoneSomething;
}

static VOID
EvtchnClose(
    IN  PINTERFACE              Interface,
    IN  PXENBUS_EVTCHN_CHANNEL  Channel
    )
{
    PXENBUS_EVTCHN_CONTEXT      Context = Interface->Context;
    KIRQL                       Irql;

    ASSERT3U(Channel->Magic, ==, XENBUS_EVTCHN_CHANNEL_MAGIC);

    KeRaiseIrql(DISPATCH_LEVEL, &Irql); // Prevent suspend

    KeAcquireSpinLockAtDpcLevel(&Context->Lock);

    RemoveEntryList(&Channel->ListEntry);

    if (IsListEmpty(&Context->List) && Context->Enabled) {
        EvtchnInterruptDisable(Context);
        Context->Enabled = FALSE;
    }

    KeReleaseSpinLockFromDpcLevel(&Context->Lock);

    RtlZeroMemory(&Channel->ListEntry, sizeof (LIST_ENTRY));

    if (Channel->Active) {
        ULONG       LocalPort = Channel->LocalPort;
        NTSTATUS    status;

        Channel->Active = FALSE;

        XENBUS_EVTCHN_ABI(PortMask,
                          &Context->EvtchnAbi,
                          LocalPort);

        if (Channel->Type != XENBUS_EVTCHN_TYPE_FIXED)
            (VOID) EventChannelClose(LocalPort);

        status = HashTableRemove(Context->Table, LocalPort);
        ASSERT(NT_SUCCESS(status));
    }

    Channel->LocalPort = 0;
    RtlZeroMemory(&Channel->Parameters, sizeof (XENBUS_EVTCHN_PARAMETERS));

    Channel->Argument = NULL;
    Channel->Callback = NULL;
    Channel->Type = 0;

    Channel->Caller = NULL;

    Channel->Magic = 0;

    ASSERT(IsZeroMemory(Channel, sizeof (XENBUS_EVTCHN_CHANNEL)));
    __EvtchnFree(Channel);

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

static BOOLEAN
EvtchnPollCallback(
    IN  PVOID               Argument,
    IN  ULONG               LocalPort
    )
{
    PXENBUS_EVTCHN_CONTEXT  Context = Argument;
    PXENBUS_EVTCHN_CHANNEL  Channel;
    BOOLEAN                 DoneSomething;
    NTSTATUS                status;

    status = HashTableLookup(Context->Table,
                             LocalPort,
                             (PULONG_PTR)&Channel);
    
    if (!NT_SUCCESS(status)) {
        Warning("[%d]: INVALID PORT\n", LocalPort);

        XENBUS_EVTCHN_ABI(PortMask,
                          &Context->EvtchnAbi,
                          LocalPort);

        DoneSomething = FALSE;
        goto done;
    }

    if (Channel->Mask)
        XENBUS_EVTCHN_ABI(PortMask,
                          &Context->EvtchnAbi,
                          LocalPort);

    XENBUS_EVTCHN_ABI(PortAck,
                      &Context->EvtchnAbi,
                      LocalPort);

    DoneSomething = EvtchnCallback(Context, Channel);

done:
    return DoneSomething;
}

BOOLEAN
EvtchnInterrupt(
    IN  PXENBUS_EVTCHN_CONTEXT  Context
    )
{
    BOOLEAN                     DoneSomething;

    DoneSomething = FALSE;

    while (XENBUS_SHARED_INFO(UpcallPending,
                              &Context->SharedInfoInterface,
                              0))
        DoneSomething |= XENBUS_EVTCHN_ABI(Poll,
                                           &Context->EvtchnAbi,
                                           0,
                                           EvtchnPollCallback,
                                           Context);

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

    if (Context->Enabled)
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
    NTSTATUS                status;

    KeAcquireSpinLock(&Context->Lock, &Irql);

    if (Context->References++ != 0)
        goto done;

    Trace("====>\n");

    Context->Vector = FdoGetInterruptVector(Fdo);

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

    Trace("<====\n");

done:
    KeReleaseSpinLock(&Context->Lock, Irql);

    return STATUS_SUCCESS;

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

    Context->Vector = 0;

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
    KIRQL                   Irql;

    KeAcquireSpinLock(&Context->Lock, &Irql);

    if (--Context->References > 0)
        goto done;

    Trace("====>\n");

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

    Context->Vector = 0;

    Trace("<====\n");

done:
    KeReleaseSpinLock(&Context->Lock, Irql);
}

static struct _XENBUS_EVTCHN_INTERFACE_V1 EvtchnInterfaceVersion1 = {
    { sizeof (struct _XENBUS_EVTCHN_INTERFACE_V1), 1, NULL, NULL, NULL },
    EvtchnAcquire,
    EvtchnRelease,
    EvtchnOpen,
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

    Context->Fdo = NULL;

    RtlZeroMemory(&Context->Lock, sizeof (KSPIN_LOCK));
    RtlZeroMemory(&Context->List, sizeof (LIST_ENTRY));

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
