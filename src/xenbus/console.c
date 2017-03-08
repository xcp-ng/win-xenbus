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
#include <ntstrsafe.h>
#include <stdarg.h>
#include <stdlib.h>
#include <xen.h>

#include "console.h"
#include "evtchn.h"
#include "fdo.h"
#include "high.h"
#include "dbg_print.h"
#include "assert.h"
#include "util.h"

typedef struct _XENBUS_CONSOLE_BUFFER {
    LIST_ENTRY  ListEntry;
    ULONG       Offset;
    ULONG       Length;
    CHAR        Data[1];    // Variable length array
} XENBUS_CONSOLE_BUFFER, *PXENBUS_CONSOLE_BUFFER;

struct _XENBUS_CONSOLE_CONTEXT {
    PXENBUS_FDO                 Fdo;
    KSPIN_LOCK                  Lock;
    LONG                        References;
    struct xencons_interface    *Shared;
    HIGH_LOCK                   RingLock;
    XENBUS_EVTCHN_INTERFACE     EvtchnInterface;
    PHYSICAL_ADDRESS            Address;
    PXENBUS_EVTCHN_CHANNEL      Channel;
    ULONG                       Events;
    XENBUS_SUSPEND_INTERFACE    SuspendInterface;
    XENBUS_DEBUG_INTERFACE      DebugInterface;
    PXENBUS_SUSPEND_CALLBACK    SuspendCallbackEarly;
    PXENBUS_SUSPEND_CALLBACK    SuspendCallbackLate;
    PXENBUS_DEBUG_CALLBACK      DebugCallback;
    BOOLEAN                     Enabled;
};

C_ASSERT(sizeof (struct xencons_interface) <= PAGE_SIZE);

#define XENBUS_CONSOLE_TAG  'SNOC'

static FORCEINLINE PVOID
__ConsoleAllocate(
    IN  ULONG   Length
    )
{
    return __AllocatePoolWithTag(NonPagedPool, Length, XENBUS_CONSOLE_TAG);
}

static FORCEINLINE VOID
__ConsoleFree(
    IN  PVOID   Buffer
    )
{
    __FreePoolWithTag(Buffer, XENBUS_CONSOLE_TAG);
}

static ULONG
ConsoleCopyToRing(
    IN  PXENBUS_CONSOLE_CONTEXT Context,
    IN  PCHAR                   Data,
    IN  ULONG                   Length,
    IN  BOOLEAN                 CRLF
    )
{
    struct xencons_interface    *Shared;
    XENCONS_RING_IDX            cons;
    XENCONS_RING_IDX            prod;
    ULONG                       Offset;

    Shared = Context->Shared;

    KeMemoryBarrier();

    prod = Shared->out_prod;
    cons = Shared->out_cons;

    KeMemoryBarrier();

    Offset = 0;
    while (Length != 0) {
        CHAR    Character = Data[Offset];
        ULONG   Required;
        ULONG   Available;
        ULONG   Index;

        Required = (CRLF && Character == '\n') ? 2 : 1;
        Available = cons + sizeof (Shared->out) - prod;

        if (Available < Required)
            break;

        Index = MASK_XENCONS_IDX(prod, Shared->out);

        if (CRLF && Character == '\n') {
            Shared->out[Index] = '\r';
            prod++;

            Index = MASK_XENCONS_IDX(prod, Shared->out);
        }

        Shared->out[Index] = Character;
        prod++;

        Offset++;
        Length--;
    }

    KeMemoryBarrier();

    Shared->out_prod = prod;

    KeMemoryBarrier();

    return Offset;
}

static
_Function_class_(KSERVICE_ROUTINE)
_IRQL_requires_(HIGH_LEVEL)
_IRQL_requires_same_
BOOLEAN
ConsoleEvtchnCallback(
    IN  PKINTERRUPT         InterruptObject,
    IN  PVOID               Argument
    )
{
    PXENBUS_CONSOLE_CONTEXT Context = Argument;

    UNREFERENCED_PARAMETER(InterruptObject);

    ASSERT(Context != NULL);

    Context->Events++;

    return TRUE;
}

static VOID
ConsoleDisable(
    IN PXENBUS_CONSOLE_CONTEXT  Context
    )
{
    LogPrintf(LOG_LEVEL_INFO,
              "CONSOLE: DISABLE\n");

    Context->Enabled = FALSE;

    XENBUS_EVTCHN(Close,
                  &Context->EvtchnInterface,
                  Context->Channel);
    Context->Channel = NULL;
}

static VOID
ConsoleEnable(
    IN PXENBUS_CONSOLE_CONTEXT  Context
    )
{
    ULONGLONG                   Value;
    ULONG                       Port;
    NTSTATUS                    status;

    status = HvmGetParam(HVM_PARAM_CONSOLE_EVTCHN, &Value);
    ASSERT(NT_SUCCESS(status));

    Port = (ULONG)Value;

    Context->Channel = XENBUS_EVTCHN(Open,
                                     &Context->EvtchnInterface,
                                     XENBUS_EVTCHN_TYPE_FIXED,
                                     ConsoleEvtchnCallback,
                                     Context,
                                     Port,
                                     FALSE);
    ASSERT(Context->Channel != NULL);

    XENBUS_EVTCHN(Unmask,
                  &Context->EvtchnInterface,
                  Context->Channel,
                  FALSE);

    Context->Enabled = TRUE;

    LogPrintf(LOG_LEVEL_INFO,
              "CONSOLE: ENABLE (%u)\n",
              Port);
}

static PHYSICAL_ADDRESS
ConsoleGetAddress(
    PXENBUS_CONSOLE_CONTEXT Context
    )
{
    PHYSICAL_ADDRESS        Address;
    NTSTATUS                status;

    UNREFERENCED_PARAMETER(Context);

    status = HvmGetParam(HVM_PARAM_CONSOLE_PFN,
                         (PULONGLONG)&Address.QuadPart);
    ASSERT(NT_SUCCESS(status));

    Address.QuadPart <<= PAGE_SHIFT;

    LogPrintf(LOG_LEVEL_INFO,
              "CONSOLE: PAGE @ %08x.%08x\n",
              Address.HighPart,
              Address.LowPart);

    return Address;
}

static VOID
ConsoleSuspendCallbackEarly(
    IN  PVOID               Argument
    )
{
    PXENBUS_CONSOLE_CONTEXT Context = Argument;
    PHYSICAL_ADDRESS        Address;

    Address = ConsoleGetAddress(Context);
    ASSERT3U(Address.QuadPart, ==, Context->Address.QuadPart);
}

static VOID
ConsoleSuspendCallbackLate(
    IN  PVOID                   Argument
    )
{
    PXENBUS_CONSOLE_CONTEXT     Context = Argument;
    struct xencons_interface    *Shared;
    KIRQL                       Irql;

    Shared = Context->Shared;

    KeAcquireSpinLock(&Context->Lock, &Irql);

    ConsoleDisable(Context);
    ConsoleEnable(Context);

    KeReleaseSpinLock(&Context->Lock, Irql);
}

static VOID
ConsoleDebugCallback(
    IN  PVOID               Argument,
    IN  BOOLEAN             Crashing
    )
{
    PXENBUS_CONSOLE_CONTEXT Context = Argument;

    XENBUS_DEBUG(Printf,
                 &Context->DebugInterface,
                 "Address = %08x.%08x\n",
                 Context->Address.HighPart,
                 Context->Address.LowPart);

    if (!Crashing) {
        struct xencons_interface    *Shared;

        Shared = Context->Shared;

        XENBUS_DEBUG(Printf,
                     &Context->DebugInterface,
                     "out_cons = %08x out_prod = %08x\n",
                     Shared->out_cons,
                     Shared->out_prod);

        XENBUS_DEBUG(Printf,
                     &Context->DebugInterface,
                     "in_cons = %08x in_prod = %08x\n",
                     Shared->in_cons,
                     Shared->in_prod);
    }

    XENBUS_DEBUG(Printf,
                 &Context->DebugInterface,
                 "Events = %lu\n",
                 Context->Events);
}

static ULONG
ConsoleWrite(
    IN  PINTERFACE          Interface,
    IN  PCHAR               Data,
    IN  ULONG               Length,
    IN  BOOLEAN             CRLF
    )
{
    PXENBUS_CONSOLE_CONTEXT Context = Interface->Context;
    KIRQL                   Irql;
    ULONG                   Written;
    NTSTATUS                status;

    AcquireHighLock(&Context->RingLock, &Irql);

    Written = 0;

    status = STATUS_UNSUCCESSFUL;
    if (!Context->Enabled)
        goto done;

    Written += ConsoleCopyToRing(Context, Data, Length, CRLF);

    if (Written != 0)
        XENBUS_EVTCHN(Send,
                      &Context->EvtchnInterface,
                      Context->Channel);

done:
    ReleaseHighLock(&Context->RingLock, Irql);

    return Written;
}

static NTSTATUS
ConsoleAcquire(
    IN  PINTERFACE          Interface
    )
{
    PXENBUS_CONSOLE_CONTEXT Context = Interface->Context;
    KIRQL                   Irql;
    NTSTATUS                status;

    KeAcquireSpinLock(&Context->Lock, &Irql);

    if (Context->References++ != 0)
        goto done;

    Trace("====>\n");

    Context->Address = ConsoleGetAddress(Context);
    Context->Shared = (struct xencons_interface *)MmMapIoSpace(Context->Address,
                                                               PAGE_SIZE,
                                                               MmCached);
    status = STATUS_UNSUCCESSFUL;
    if (Context->Shared == NULL)
        goto fail1;

    status = XENBUS_EVTCHN(Acquire, &Context->EvtchnInterface);
    if (!NT_SUCCESS(status))
        goto fail2;

    ConsoleEnable(Context);

    status = XENBUS_SUSPEND(Acquire, &Context->SuspendInterface);
    if (!NT_SUCCESS(status))
        goto fail3;

    status = XENBUS_SUSPEND(Register,
                            &Context->SuspendInterface,
                            SUSPEND_CALLBACK_EARLY,
                            ConsoleSuspendCallbackEarly,
                            Context,
                            &Context->SuspendCallbackEarly);
    if (!NT_SUCCESS(status))
        goto fail4;

    status = XENBUS_SUSPEND(Register,
                            &Context->SuspendInterface,
                            SUSPEND_CALLBACK_LATE,
                            ConsoleSuspendCallbackLate,
                            Context,
                            &Context->SuspendCallbackLate);
    if (!NT_SUCCESS(status))
        goto fail5;

    status = XENBUS_DEBUG(Acquire, &Context->DebugInterface);
    if (!NT_SUCCESS(status))
        goto fail6;

    status = XENBUS_DEBUG(Register,
                          &Context->DebugInterface,
                          __MODULE__ "|CONSOLE",
                          ConsoleDebugCallback,
                          Context,
                          &Context->DebugCallback);
    if (!NT_SUCCESS(status))
        goto fail7;

    Trace("<====\n");

done:
    KeReleaseSpinLock(&Context->Lock, Irql);

    return STATUS_SUCCESS;

fail7:
    Error("fail7\n");

    XENBUS_DEBUG(Release, &Context->DebugInterface);

fail6:
    Error("fail6\n");

    XENBUS_SUSPEND(Deregister,
                   &Context->SuspendInterface,
                   Context->SuspendCallbackLate);
    Context->SuspendCallbackLate = NULL;

fail5:
    Error("fail5\n");

    XENBUS_SUSPEND(Deregister,
                   &Context->SuspendInterface,
                   Context->SuspendCallbackEarly);
    Context->SuspendCallbackEarly = NULL;

fail4:
    Error("fail4\n");

    XENBUS_SUSPEND(Release, &Context->SuspendInterface);

fail3:
    Error("fail3\n");

    ConsoleDisable(Context);

    XENBUS_EVTCHN(Release, &Context->EvtchnInterface);

fail2:
    Error("fail2\n");

    MmUnmapIoSpace(Context->Shared, PAGE_SIZE);
    Context->Shared = NULL;

fail1:
    Error("fail1 (%08x)\n", status);

    Context->Address.QuadPart = 0;

    --Context->References;
    ASSERT3U(Context->References, ==, 0);
    KeReleaseSpinLock(&Context->Lock, Irql);

    return status;
}

static VOID
ConsoleRelease(
    IN  PINTERFACE          Interface
    )
{
    PXENBUS_CONSOLE_CONTEXT Context = Interface->Context;
    KIRQL                   Irql;

    KeAcquireSpinLock(&Context->Lock, &Irql);

    if (--Context->References > 0)
        goto done;

    Trace("====>\n");

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

    ConsoleDisable(Context);

    XENBUS_EVTCHN(Release, &Context->EvtchnInterface);

    MmUnmapIoSpace(Context->Shared, PAGE_SIZE);
    Context->Shared = NULL;

    Context->Address.QuadPart = 0;

    Trace("<====\n");

done:
    KeReleaseSpinLock(&Context->Lock, Irql);
}

static struct _XENBUS_CONSOLE_INTERFACE_V1 ConsoleInterfaceVersion1 = {
    { sizeof (struct _XENBUS_CONSOLE_INTERFACE_V1), 1, NULL, NULL, NULL },
    ConsoleAcquire,
    ConsoleRelease,
    ConsoleWrite
};

NTSTATUS
ConsoleInitialize(
    IN  PXENBUS_FDO             Fdo,
    OUT PXENBUS_CONSOLE_CONTEXT *Context
    )
{
    NTSTATUS                    status;

    Trace("====>\n");

    *Context = __ConsoleAllocate(sizeof (XENBUS_CONSOLE_CONTEXT));

    status = STATUS_NO_MEMORY;
    if (*Context == NULL)
        goto fail1;

    status = EvtchnGetInterface(FdoGetEvtchnContext(Fdo),
                                XENBUS_EVTCHN_INTERFACE_VERSION_MAX,
                                (PINTERFACE)&(*Context)->EvtchnInterface,
                                sizeof ((*Context)->EvtchnInterface));
    ASSERT(NT_SUCCESS(status));
    ASSERT((*Context)->EvtchnInterface.Interface.Context != NULL);

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

    KeInitializeSpinLock(&(*Context)->Lock);
    InitializeHighLock(&(*Context)->RingLock);

    (*Context)->Fdo = Fdo;

    Trace("<====\n");

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

NTSTATUS
ConsoleGetInterface(
    IN      PXENBUS_CONSOLE_CONTEXT   Context,
    IN      ULONG                   Version,
    IN OUT  PINTERFACE              Interface,
    IN      ULONG                   Size
    )
{
    NTSTATUS                        status;

    ASSERT(Context != NULL);

    switch (Version) {
    case 1: {
        struct _XENBUS_CONSOLE_INTERFACE_V1  *ConsoleInterface;

        ConsoleInterface = (struct _XENBUS_CONSOLE_INTERFACE_V1 *)Interface;

        status = STATUS_BUFFER_OVERFLOW;
        if (Size < sizeof (struct _XENBUS_CONSOLE_INTERFACE_V1))
            break;

        *ConsoleInterface = ConsoleInterfaceVersion1;

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
ConsoleGetReferences(
    IN  PXENBUS_CONSOLE_CONTEXT   Context
    )
{
    return Context->References;
}

VOID
ConsoleTeardown(
    IN  PXENBUS_CONSOLE_CONTEXT   Context
    )
{
    Trace("====>\n");

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    Context->Events = 0;

    Context->Fdo = NULL;

    RtlZeroMemory(&Context->RingLock, sizeof (HIGH_LOCK));
    RtlZeroMemory(&Context->Lock, sizeof (KSPIN_LOCK));

    RtlZeroMemory(&Context->DebugInterface,
                  sizeof (XENBUS_DEBUG_INTERFACE));

    RtlZeroMemory(&Context->SuspendInterface,
                  sizeof (XENBUS_SUSPEND_INTERFACE));

    RtlZeroMemory(&Context->EvtchnInterface,
                  sizeof (XENBUS_EVTCHN_INTERFACE));

    ASSERT(IsZeroMemory(Context, sizeof (XENBUS_CONSOLE_CONTEXT)));
    __ConsoleFree(Context);

    Trace("<====\n");
}
