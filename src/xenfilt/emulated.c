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
#include <stdlib.h>
#include <stdarg.h>
#include <xen.h>

#include "registry.h"
#include "emulated.h"
#include "dbg_print.h"
#include "assert.h"
#include "util.h"

#define MAXNAMELEN  128

typedef struct _XENFILT_EMULATED_DEVICE_DATA {
    CHAR    DeviceID[MAXNAMELEN];
    CHAR    InstanceID[MAXNAMELEN];
    CHAR    CompatibleID[MAXNAMELEN];
} XENFILT_EMULATED_DEVICE_DATA, *PXENFILT_EMULATED_DEVICE_DATA;

typedef struct _XENFILT_EMULATED_DISK_DATA {
    ULONG   Index;
} XENFILT_EMULATED_DISK_DATA, *PXENFILT_EMULATED_DISK_DATA;

typedef union _XENFILT_EMULATED_OBJECT_DATA {
    XENFILT_EMULATED_DEVICE_DATA Device;
    XENFILT_EMULATED_DISK_DATA   Disk;
} XENFILT_EMULATED_OBJECT_DATA, *PXENFILT_EMULATED_OBJECT_DATA;

struct _XENFILT_EMULATED_OBJECT {
    LIST_ENTRY                      ListEntry;
    XENFILT_EMULATED_OBJECT_TYPE    Type;
    XENFILT_EMULATED_OBJECT_DATA    Data;
};

struct _XENFILT_EMULATED_CONTEXT {
    KSPIN_LOCK          Lock;
    LONG                References;
    LIST_ENTRY          List;
};

#define XENFILT_EMULATED_TAG    'LUME'

static FORCEINLINE PVOID
__EmulatedAllocate(
    _In_ ULONG  Length
    )
{
    return __AllocatePoolWithTag(NonPagedPool, Length, XENFILT_EMULATED_TAG);
}

static FORCEINLINE VOID
__EmulatedFree(
    _In_ PVOID  Buffer
    )
{
    __FreePoolWithTag(Buffer, XENFILT_EMULATED_TAG);
}

static NTSTATUS
EmulatedSetObjectDeviceData(
    _In_ PXENFILT_EMULATED_OBJECT       EmulatedObject,
    _In_ XENFILT_EMULATED_OBJECT_TYPE   Type,
    _In_ PSTR                           DeviceID,
    _In_ PSTR                           InstanceID,
    _In_opt_ PSTR                       CompatibleIDs
    )
{
    ULONG                               Index;
    PSTR                                LastMatch;
    NTSTATUS                            status;

    status = STATUS_INVALID_PARAMETER;
    if (Type != XENFILT_EMULATED_OBJECT_TYPE_PCI)
        goto fail1;

    status = RtlStringCbPrintfA(EmulatedObject->Data.Device.DeviceID,
                                MAXNAMELEN,
                                "%s",
                                DeviceID);
    ASSERT(NT_SUCCESS(status));

    status = RtlStringCbPrintfA(EmulatedObject->Data.Device.InstanceID,
                                MAXNAMELEN,
                                "%s",
                                InstanceID);
    ASSERT(NT_SUCCESS(status));

    if (CompatibleIDs == NULL)
        goto done;

    Index = 0;
    LastMatch = CompatibleIDs;
    for (;;) {
        ULONG   Length;

        Length = (ULONG)strlen(&CompatibleIDs[Index]);
        if (Length == 0)
            break;

        LastMatch = &CompatibleIDs[Index];

        Index += Length + 1;
    }

    status = RtlStringCbPrintfA(EmulatedObject->Data.Device.CompatibleID,
                                MAXNAMELEN,
                                "%s",
                                LastMatch);
    ASSERT(NT_SUCCESS(status));

done:
    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static NTSTATUS
EmulatedSetObjectDiskData(
    _In_ PXENFILT_EMULATED_OBJECT       EmulatedObject,
    _In_ XENFILT_EMULATED_OBJECT_TYPE   Type,
    _In_ PSTR                           DeviceID,
    _In_ PSTR                           InstanceID,
    _In_opt_ PSTR                       CompatibleIDs
    )
{
    PSTR                                End;
    ULONG                               Controller;
    ULONG                               Target;
    ULONG                               Lun;
    NTSTATUS                            status;

    UNREFERENCED_PARAMETER(DeviceID);
    UNREFERENCED_PARAMETER(CompatibleIDs);

    status = STATUS_INVALID_PARAMETER;
    if (Type != XENFILT_EMULATED_OBJECT_TYPE_IDE)
        goto fail1;

    Controller = strtoul(InstanceID, &End, 10);

    status = STATUS_INVALID_PARAMETER;
    if (Controller > 1 || *End != '.')
        goto fail2;

    End++;

    Target = strtoul(End, &End, 10);

    status = STATUS_INVALID_PARAMETER;
    if (Target > 1 || *End != '.')
        goto fail3;

    End++;

    Lun = strtoul(End, &End, 10);

    status = STATUS_NOT_SUPPORTED;
    if (Lun != 0)
        goto fail4;

    status = STATUS_INVALID_PARAMETER;
    if (*End != '\0')
        goto fail5;

    EmulatedObject->Data.Disk.Index = Controller << 1 | Target;

    return STATUS_SUCCESS;

fail5:
    Error("fail5\n");

fail4:
    Error("fail4\n");

fail3:
    Error("fail3\n");

fail2:
    Error("fail2\n");

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

NTSTATUS
EmulatedAddObject(
    _In_ PXENFILT_EMULATED_CONTEXT      Context,
    _In_ PSTR                           DeviceID,
    _In_ PSTR                           InstanceID,
    _In_opt_ PSTR                       CompatibleIDs,
    _In_ XENFILT_EMULATED_OBJECT_TYPE   Type,
    _Outptr_ PXENFILT_EMULATED_OBJECT   *EmulatedObject
    )
{
    KIRQL                               Irql;
    NTSTATUS                            status;

    Trace("====>\n");

    *EmulatedObject = __EmulatedAllocate(sizeof (XENFILT_EMULATED_OBJECT));

    status = STATUS_NO_MEMORY;
    if (*EmulatedObject == NULL)
        goto fail1;

    switch (Type) {
    case XENFILT_EMULATED_OBJECT_TYPE_PCI:
        status = EmulatedSetObjectDeviceData(*EmulatedObject,
                                             Type,
                                             DeviceID,
                                             InstanceID,
                                             CompatibleIDs);
        break;

    case XENFILT_EMULATED_OBJECT_TYPE_IDE:
        status = EmulatedSetObjectDiskData(*EmulatedObject,
                                           Type,
                                           DeviceID,
                                           InstanceID,
                                           CompatibleIDs);
        break;

    default:
        status = STATUS_INVALID_PARAMETER;
        break;
    }

    if (!NT_SUCCESS(status))
        goto fail2;

    (*EmulatedObject)->Type = Type;

    KeAcquireSpinLock(&Context->Lock, &Irql);
    InsertTailList(&Context->List, &(*EmulatedObject)->ListEntry);
    KeReleaseSpinLock(&Context->Lock, Irql);

    Trace("<====\n");

    return STATUS_SUCCESS;

fail2:
    Error("fail2\n");

    __EmulatedFree(*EmulatedObject);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

VOID
EmulatedRemoveObject(
    _In_ PXENFILT_EMULATED_CONTEXT  Context,
    _In_ PXENFILT_EMULATED_OBJECT   EmulatedObject
    )
{
    KIRQL                           Irql;

    KeAcquireSpinLock(&Context->Lock, &Irql);
    RemoveEntryList(&EmulatedObject->ListEntry);
    KeReleaseSpinLock(&Context->Lock, Irql);

    __EmulatedFree(EmulatedObject);
}

static BOOLEAN
EmulatedIsDevicePresent(
    _In_ PINTERFACE             Interface,
    _In_ PSTR                   DeviceID,
    _In_opt_ PSTR               InstanceID
    )
{
    PXENFILT_EMULATED_CONTEXT   Context = Interface->Context;
    KIRQL                       Irql;
    PLIST_ENTRY                 ListEntry;

    Trace("====> (%s %s)\n",
          DeviceID,
          (InstanceID != NULL) ? InstanceID : "ANY");

    KeAcquireSpinLock(&Context->Lock, &Irql);

    ListEntry = Context->List.Flink;
    while (ListEntry != &Context->List) {
        PXENFILT_EMULATED_OBJECT    EmulatedObject;

        EmulatedObject = CONTAINING_RECORD(ListEntry,
                                           XENFILT_EMULATED_OBJECT,
                                           ListEntry);

        if (EmulatedObject->Type == XENFILT_EMULATED_OBJECT_TYPE_PCI &&
            _stricmp(DeviceID, EmulatedObject->Data.Device.DeviceID) == 0 &&
            (InstanceID == NULL ||
             _stricmp(InstanceID, EmulatedObject->Data.Device.InstanceID) == 0)) {
            Trace("FOUND\n");
            break;
        }

        ListEntry = ListEntry->Flink;
    }

    KeReleaseSpinLock(&Context->Lock, Irql);

    Trace("<====\n");

    return (ListEntry != &Context->List) ? TRUE : FALSE;
}

static BOOLEAN
EmulatedIsDiskPresent(
    _In_ PINTERFACE             Interface,
    _In_ ULONG                  Index
    )
{
    PXENFILT_EMULATED_CONTEXT   Context = Interface->Context;
    KIRQL                       Irql;
    PLIST_ENTRY                 ListEntry;

    Trace("====> (%02X)\n", Index);

    KeAcquireSpinLock(&Context->Lock, &Irql);

    ListEntry = Context->List.Flink;
    while (ListEntry != &Context->List) {
        PXENFILT_EMULATED_OBJECT    EmulatedObject;

        EmulatedObject = CONTAINING_RECORD(ListEntry,
                                           XENFILT_EMULATED_OBJECT,
                                           ListEntry);

        if (EmulatedObject->Type == XENFILT_EMULATED_OBJECT_TYPE_IDE &&
            Index == EmulatedObject->Data.Disk.Index) {
            Trace("FOUND\n");
            break;
        }

        if (EmulatedObject->Type == XENFILT_EMULATED_OBJECT_TYPE_PCI &&
            _stricmp("PCI\\CC_0108", EmulatedObject->Data.Device.CompatibleID) == 0 &&
            Index <= 3) {
            Trace("FOUND\n");
            break;
        }

        ListEntry = ListEntry->Flink;
    }

    KeReleaseSpinLock(&Context->Lock, Irql);

    Trace("<====\n");

    return (ListEntry != &Context->List) ? TRUE : FALSE;
}

static BOOLEAN
EmulatedIsDiskPresentVersion1(
    _In_ PINTERFACE             Interface,
    _In_ ULONG                  Controller,
    _In_ ULONG                  Target,
    _In_ ULONG                  Lun
    )
{
    UNREFERENCED_PARAMETER(Controller);
    UNREFERENCED_PARAMETER(Lun);

    //
    // XENVBD erroneously passes the disk number of the PV disk as
    // the IDE target number (i.e. in can pass a value > 1), with
    // Controller always set to 0. So, simply treat the Target argument
    // as the PV disk number and call the new method.
    //
    return EmulatedIsDiskPresent(Interface, Target);
}

NTSTATUS
EmulatedAcquire(
    _In_ PINTERFACE             Interface
    )
{
    PXENFILT_EMULATED_CONTEXT   Context = Interface->Context;
    KIRQL                       Irql;

    KeAcquireSpinLock(&Context->Lock, &Irql);

    if (Context->References++ != 0)
        goto done;

    Trace("<===>\n");

done:
    KeReleaseSpinLock(&Context->Lock, Irql);

    return STATUS_SUCCESS;
}

VOID
EmulatedRelease(
    _In_ PINTERFACE             Interface
    )
{
    PXENFILT_EMULATED_CONTEXT   Context = Interface->Context;
    KIRQL                       Irql;

    KeAcquireSpinLock(&Context->Lock, &Irql);

    if (--Context->References > 0)
        goto done;

    Trace("<===>\n");

done:
    KeReleaseSpinLock(&Context->Lock, Irql);
}

static struct _XENFILT_EMULATED_INTERFACE_V1 EmulatedInterfaceVersion1 = {
    { sizeof (struct _XENFILT_EMULATED_INTERFACE_V1), 1, NULL, NULL, NULL },
    EmulatedAcquire,
    EmulatedRelease,
    EmulatedIsDevicePresent,
    EmulatedIsDiskPresentVersion1
};

static struct _XENFILT_EMULATED_INTERFACE_V2 EmulatedInterfaceVersion2 = {
    { sizeof (struct _XENFILT_EMULATED_INTERFACE_V2), 2, NULL, NULL, NULL },
    EmulatedAcquire,
    EmulatedRelease,
    EmulatedIsDevicePresent,
    EmulatedIsDiskPresent
};

NTSTATUS
EmulatedInitialize(
    _Outptr_ PXENFILT_EMULATED_CONTEXT  *Context
    )
{
    NTSTATUS                            status;

    Trace("====>\n");

    *Context = __EmulatedAllocate(sizeof (XENFILT_EMULATED_CONTEXT));

    status = STATUS_NO_MEMORY;
    if (*Context == NULL)
        goto fail1;

    InitializeListHead(&(*Context)->List);
    KeInitializeSpinLock(&(*Context)->Lock);

    Trace("<====\n");

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

NTSTATUS
EmulatedGetInterface(
    _In_ PXENFILT_EMULATED_CONTEXT  Context,
    _In_ ULONG                      Version,
    _Inout_ PINTERFACE              Interface,
    _In_ ULONG                      Size
    )
{
    NTSTATUS                        status;

    ASSERT(Context != NULL);

    switch (Version) {
    case 1: {
        struct _XENFILT_EMULATED_INTERFACE_V1   *EmulatedInterface;

        EmulatedInterface = (struct _XENFILT_EMULATED_INTERFACE_V1 *)Interface;

        status = STATUS_BUFFER_OVERFLOW;
        if (Size < sizeof (struct _XENFILT_EMULATED_INTERFACE_V1))
            break;

        *EmulatedInterface = EmulatedInterfaceVersion1;

        ASSERT3U(Interface->Version, ==, Version);
        Interface->Context = Context;

        status = STATUS_SUCCESS;
        break;
    }
    case 2: {
        struct _XENFILT_EMULATED_INTERFACE_V2   *EmulatedInterface;

        EmulatedInterface = (struct _XENFILT_EMULATED_INTERFACE_V2 *)Interface;

        status = STATUS_BUFFER_OVERFLOW;
        if (Size < sizeof (struct _XENFILT_EMULATED_INTERFACE_V2))
            break;

        *EmulatedInterface = EmulatedInterfaceVersion2;

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
EmulatedTeardown(
    _In_ PXENFILT_EMULATED_CONTEXT  Context
    )
{
    Trace("====>\n");

    if (!IsListEmpty(&Context->List))
        BUG("OUTSTANDING OBJECTS");

    RtlZeroMemory(&Context->Lock, sizeof (KSPIN_LOCK));
    RtlZeroMemory(&Context->List, sizeof (LIST_ENTRY));

    ASSERT(IsZeroMemory(Context, sizeof (XENFILT_EMULATED_CONTEXT)));
    __EmulatedFree(Context);

    Trace("<====\n");
}
