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
} XENFILT_EMULATED_DEVICE_DATA, *PXENFILT_EMULATED_DEVICE_DATA;

typedef struct _XENFILT_EMULATED_DISK_DATA {
    ULONG   Controller;
    ULONG   Target;
    ULONG   Lun;
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
    IN  ULONG   Length
    )
{
    return __AllocatePoolWithTag(NonPagedPool, Length, XENFILT_EMULATED_TAG);
}

static FORCEINLINE VOID
__EmulatedFree(
    IN  PVOID   Buffer
    )
{
    ExFreePoolWithTag(Buffer, XENFILT_EMULATED_TAG);
}

static NTSTATUS
EmulatedSetObjectDeviceData(
    IN  PXENFILT_EMULATED_OBJECT    EmulatedObject,
    IN  PWCHAR                      DeviceID,
    IN  PWCHAR                      InstanceID
    )
{
    NTSTATUS                        status;

    status = RtlStringCbPrintfA(EmulatedObject->Data.Device.DeviceID,
                                MAXNAMELEN,
                                "%ws",
                                DeviceID);
    ASSERT(NT_SUCCESS(status));

    status = RtlStringCbPrintfA(EmulatedObject->Data.Device.InstanceID,
                                MAXNAMELEN,
                                "%ws",
                                InstanceID);
    ASSERT(NT_SUCCESS(status));

    return STATUS_SUCCESS;
}

static NTSTATUS
EmulatedSetObjectDiskData(
    IN  PXENFILT_EMULATED_OBJECT    EmulatedObject,
    IN  PWCHAR                      DeviceID,
    IN  PWCHAR                      InstanceID
    )
{
    UNICODE_STRING                  Unicode;
    ANSI_STRING                     Ansi;
    PCHAR                           End;
    ULONG                           Controller;
    ULONG                           Target;
    ULONG                           Lun;
    NTSTATUS                        status;

    UNREFERENCED_PARAMETER(DeviceID);

    RtlInitUnicodeString(&Unicode, InstanceID);

    status = RtlUnicodeStringToAnsiString(&Ansi, &Unicode, TRUE);
    if (!NT_SUCCESS(status))
        goto fail1;

    Controller = strtol(Ansi.Buffer, &End, 10);

    status = STATUS_INVALID_PARAMETER;
    if (*End != '.')
        goto fail2;

    End++;

    Target = strtol(End, &End, 10);

    status = STATUS_INVALID_PARAMETER;
    if (*End != '.')
        goto fail3;

    End++;

    Lun = strtol(End, &End, 10);

    status = STATUS_INVALID_PARAMETER;
    if (*End != '\0')
        goto fail4;

    EmulatedObject->Data.Disk.Controller = Controller;
    EmulatedObject->Data.Disk.Target = Target;
    EmulatedObject->Data.Disk.Lun = Lun;

    RtlFreeAnsiString(&Ansi);

    return STATUS_SUCCESS;

fail4:
    Error("fail4\n");

fail3:
    Error("fail3\n");

fail2:
    Error("fail2\n");

    RtlFreeAnsiString(&Ansi);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

NTSTATUS
EmulatedAddObject(
    IN  PXENFILT_EMULATED_CONTEXT       Context,
    IN  PWCHAR                          DeviceID,
    IN  PWCHAR                          InstanceID,
    IN  XENFILT_EMULATED_OBJECT_TYPE    Type,
    OUT PXENFILT_EMULATED_OBJECT        *EmulatedObject
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
    case XENFILT_EMULATED_OBJECT_TYPE_DEVICE:
        status = EmulatedSetObjectDeviceData(*EmulatedObject,
                                             DeviceID,
                                             InstanceID);
        break;

    case XENFILT_EMULATED_OBJECT_TYPE_DISK:
        status = EmulatedSetObjectDiskData(*EmulatedObject,
                                           DeviceID,
                                           InstanceID);
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
    IN  PXENFILT_EMULATED_CONTEXT   Context,
    IN  PXENFILT_EMULATED_OBJECT    EmulatedObject
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
    IN  PINTERFACE              Interface,
    IN  PCHAR                   DeviceID,
    IN  PCHAR                   InstanceID OPTIONAL
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

        if (EmulatedObject->Type == XENFILT_EMULATED_OBJECT_TYPE_DEVICE &&
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
    IN  PINTERFACE              Interface,
    IN  ULONG                   Controller,
    IN  ULONG                   Target,
    IN  ULONG                   Lun
    )
{
    PXENFILT_EMULATED_CONTEXT   Context = Interface->Context;
    KIRQL                       Irql;
    PLIST_ENTRY                 ListEntry;

    Trace("====> (%02X:%02X:%02X)\n", Controller, Target, Lun);

    KeAcquireSpinLock(&Context->Lock, &Irql);

    ListEntry = Context->List.Flink;
    while (ListEntry != &Context->List) {
        PXENFILT_EMULATED_OBJECT    EmulatedObject;

        EmulatedObject = CONTAINING_RECORD(ListEntry,
                                           XENFILT_EMULATED_OBJECT,
                                           ListEntry);

        if (EmulatedObject->Type == XENFILT_EMULATED_OBJECT_TYPE_DISK &&
            Controller == EmulatedObject->Data.Disk.Controller &&
            Target == EmulatedObject->Data.Disk.Target &&
            Lun == EmulatedObject->Data.Disk.Lun) {
            Trace("FOUND");
            break;
        }

        ListEntry = ListEntry->Flink;
    }

    KeReleaseSpinLock(&Context->Lock, Irql);

    Trace("<====\n");

    return (ListEntry != &Context->List) ? TRUE : FALSE;
}

NTSTATUS
EmulatedAcquire(
    IN  PINTERFACE              Interface
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
    IN  PINTERFACE              Interface
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
    EmulatedIsDiskPresent
};
                     
NTSTATUS
EmulatedInitialize(
    OUT PXENFILT_EMULATED_CONTEXT   *Context
    )
{
    NTSTATUS                        status;

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
    IN      PXENFILT_EMULATED_CONTEXT   Context,
    IN      ULONG                       Version,
    IN OUT  PINTERFACE                  Interface,
    IN      ULONG                       Size
    )
{
    NTSTATUS                            status;

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
    default:
        status = STATUS_NOT_SUPPORTED;
        break;
    }

    return status;
}   

VOID
EmulatedTeardown(
    IN  PXENFILT_EMULATED_CONTEXT   Context
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
