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
#include <ntstrsafe.h>
#include <stdarg.h>
#include <stdlib.h>
#include <xen.h>

#include "unplug.h"
#include "fdo.h"
#include "mutex.h"
#include "dbg_print.h"
#include "assert.h"
#include "util.h"
#include "registry.h"

struct _XENBUS_UNPLUG_CONTEXT {
    KSPIN_LOCK  Lock;
    LONG        References;
    MUTEX       Mutex;
};

#define XENBUS_UNPLUG_TAG    'LPNU'

static FORCEINLINE PVOID
__UnplugAllocate(
    _In_ ULONG  Length
    )
{
    return __AllocatePoolWithTag(NonPagedPool, Length, XENBUS_UNPLUG_TAG);
}

static FORCEINLINE VOID
__UnplugFree(
    _In_ PVOID  Buffer
    )
{
    __FreePoolWithTag(Buffer, XENBUS_UNPLUG_TAG);
}

_IRQL_requires_(PASSIVE_LEVEL)
static VOID
UnplugRequest(
    _In_ PINTERFACE                 Interface,
    _In_ XENBUS_UNPLUG_DEVICE_TYPE  Type,
    _In_ BOOLEAN                    Make
    )
{
    PXENBUS_UNPLUG_CONTEXT          Context = Interface->Context;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    AcquireMutex(&Context->Mutex);

    switch (Type) {
    case XENBUS_UNPLUG_DEVICE_TYPE_NICS:
        Info("NICS (%s)\n", (Make) ? "MAKE" : "REVOKE");

        if (Make)
            (VOID) UnplugIncrementValue(UNPLUG_NICS);
        else
            (VOID) UnplugDecrementValue(UNPLUG_NICS);

        break;

    case XENBUS_UNPLUG_DEVICE_TYPE_DISKS:
        Info("DISKS (%s)\n", (Make) ? "MAKE" : "REVOKE");

        if (Make)
            (VOID) UnplugIncrementValue(UNPLUG_DISKS);
        else
            (VOID) UnplugDecrementValue(UNPLUG_DISKS);

        break;

    default:
        ASSERT(FALSE);
        break;
    }

    ReleaseMutex(&Context->Mutex);
}

_IRQL_requires_(PASSIVE_LEVEL)
static BOOLEAN
UnplugIsRequested(
    _In_ PINTERFACE                 Interface,
    _In_ XENBUS_UNPLUG_DEVICE_TYPE  Type
    )
{
    PXENBUS_UNPLUG_CONTEXT          Context = Interface->Context;
    BOOLEAN                         Requested;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    AcquireMutex(&Context->Mutex);

    Requested = FALSE;
    switch (Type) {
    case XENBUS_UNPLUG_DEVICE_TYPE_NICS:
        Requested = UnplugGetRequest(UNPLUG_NICS);
        break;

    case XENBUS_UNPLUG_DEVICE_TYPE_DISKS:
        Requested = UnplugGetRequest(UNPLUG_DISKS);
        break;

    default:
        ASSERT(FALSE);
        break;
    }

    ReleaseMutex(&Context->Mutex);

    return Requested;
}

_IRQL_requires_(PASSIVE_LEVEL)
static BOOLEAN
UnplugBootEmulated(
    _In_ PINTERFACE                 Interface
    )
{
    PXENBUS_UNPLUG_CONTEXT          Context = Interface->Context;
    CHAR                            KeyName[] = "XEN:BOOT_EMULATED=";
    PANSI_STRING                    Option;
    PSTR                            Value;
    NTSTATUS                        status;
    BOOLEAN                         BootEmulated;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    AcquireMutex(&Context->Mutex);

    BootEmulated = FALSE;

    status = ConfigQuerySystemStartOption(KeyName, &Option);
    if (!NT_SUCCESS(status))
        goto done;

    Value = Option->Buffer + sizeof (KeyName) - 1;

    if (strcmp(Value, "TRUE") == 0)
        BootEmulated = TRUE;
    else if (strcmp(Value, "FALSE") != 0)
        Warning("UNRECOGNIZED VALUE OF %s: %s\n", KeyName, Value);

    RegistryFreeSzValue(Option);

done:
    ReleaseMutex(&Context->Mutex);

    return BootEmulated;
}

_IRQL_requires_(PASSIVE_LEVEL)
static VOID
UnplugReboot(
    _In_ PINTERFACE                 Interface,
    _In_ PSTR                       Module
    )
{
    PXENBUS_UNPLUG_CONTEXT          Context = Interface->Context;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    AcquireMutex(&Context->Mutex);

    ConfigRequestReboot(DriverGetParametersKey(), Module);

    ReleaseMutex(&Context->Mutex);

    return;
}

static NTSTATUS
UnplugAcquire(
    _In_ PINTERFACE         Interface
    )
{
    PXENBUS_UNPLUG_CONTEXT  Context = Interface->Context;
    KIRQL                   Irql;

    KeAcquireSpinLock(&Context->Lock, &Irql);

    if (Context->References++ != 0)
        goto done;

    Trace("<===>\n");

done:
    KeReleaseSpinLock(&Context->Lock, Irql);

    return STATUS_SUCCESS;
}

static VOID
UnplugRelease(
    _In_ PINTERFACE         Interface
    )
{
    PXENBUS_UNPLUG_CONTEXT  Context = Interface->Context;
    KIRQL                   Irql;

    KeAcquireSpinLock(&Context->Lock, &Irql);

    if (--Context->References > 0)
        goto done;

    Trace("<===>\n");

done:
    KeReleaseSpinLock(&Context->Lock, Irql);
}

static struct _XENBUS_UNPLUG_INTERFACE_V1 UnplugInterfaceVersion1 = {
    { sizeof (struct _XENBUS_UNPLUG_INTERFACE_V1), 1, NULL, NULL, NULL },
    UnplugAcquire,
    UnplugRelease,
    UnplugRequest
};

static struct _XENBUS_UNPLUG_INTERFACE_V2 UnplugInterfaceVersion2 = {
    { sizeof (struct _XENBUS_UNPLUG_INTERFACE_V2), 2, NULL, NULL, NULL },
    UnplugAcquire,
    UnplugRelease,
    UnplugRequest,
    UnplugIsRequested
};

static struct _XENBUS_UNPLUG_INTERFACE_V3 UnplugInterfaceVersion3 = {
    { sizeof (struct _XENBUS_UNPLUG_INTERFACE_V3), 3, NULL, NULL, NULL },
    UnplugAcquire,
    UnplugRelease,
    UnplugRequest,
    UnplugIsRequested,
    UnplugBootEmulated,
    UnplugReboot
};

NTSTATUS
UnplugInitialize(
    _In_ PXENBUS_FDO                Fdo,
    _Outptr_ PXENBUS_UNPLUG_CONTEXT *Context
    )
{
    NTSTATUS                        status;

    UNREFERENCED_PARAMETER(Fdo);

    Trace("====>\n");

    *Context = __UnplugAllocate(sizeof (XENBUS_UNPLUG_CONTEXT));

    status = STATUS_NO_MEMORY;
    if (*Context == NULL)
        goto fail1;

    KeInitializeSpinLock(&(*Context)->Lock);
    InitializeMutex(&(*Context)->Mutex);

    Trace("<====\n");

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

NTSTATUS
UnplugGetInterface(
    _In_ PXENBUS_UNPLUG_CONTEXT Context,
    _In_ ULONG                  Version,
    _Inout_ PINTERFACE          Interface,
    _In_ ULONG                  Size
    )
{
    NTSTATUS                    status;

    ASSERT(Context != NULL);

    switch (Version) {
    case 1: {
        struct _XENBUS_UNPLUG_INTERFACE_V1   *UnplugInterface;

        UnplugInterface = (struct _XENBUS_UNPLUG_INTERFACE_V1 *)Interface;

        status = STATUS_BUFFER_OVERFLOW;
        if (Size < sizeof (struct _XENBUS_UNPLUG_INTERFACE_V1))
            break;

        *UnplugInterface = UnplugInterfaceVersion1;

        ASSERT3U(Interface->Version, ==, Version);
        Interface->Context = Context;

        status = STATUS_SUCCESS;
        break;
    }
    case 2: {
        struct _XENBUS_UNPLUG_INTERFACE_V2   *UnplugInterface;

        UnplugInterface = (struct _XENBUS_UNPLUG_INTERFACE_V2 *)Interface;

        status = STATUS_BUFFER_OVERFLOW;
        if (Size < sizeof (struct _XENBUS_UNPLUG_INTERFACE_V2))
            break;

        *UnplugInterface = UnplugInterfaceVersion2;

        ASSERT3U(Interface->Version, ==, Version);
        Interface->Context = Context;

        status = STATUS_SUCCESS;
        break;
    }
    case 3: {
        struct _XENBUS_UNPLUG_INTERFACE_V3   *UnplugInterface;

        UnplugInterface = (struct _XENBUS_UNPLUG_INTERFACE_V3 *)Interface;

        status = STATUS_BUFFER_OVERFLOW;
        if (Size < sizeof (struct _XENBUS_UNPLUG_INTERFACE_V3))
            break;

        *UnplugInterface = UnplugInterfaceVersion3;

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
UnplugGetReferences(
    _In_ PXENBUS_UNPLUG_CONTEXT Context
    )
{
    return Context->References;
}

VOID
UnplugTeardown(
    _In_ PXENBUS_UNPLUG_CONTEXT Context
    )
{
    Trace("====>\n");

    RtlZeroMemory(&Context->Mutex, sizeof (MUTEX));
    RtlZeroMemory(&Context->Lock, sizeof (KSPIN_LOCK));

    ASSERT(IsZeroMemory(Context, sizeof (XENBUS_UNPLUG_CONTEXT)));
    __UnplugFree(Context);

    Trace("<====\n");
}
