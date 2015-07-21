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

#include "driver.h"
#include "registry.h"
#include "emulated.h"
#include "pvdevice.h"
#include "mutex.h"
#include "dbg_print.h"
#include "assert.h"
#include "util.h"

struct _XENFILT_PVDEVICE_CONTEXT {
    KSPIN_LOCK                  Lock;
    LONG                        References;
    XENFILT_EMULATED_INTERFACE  EmulatedInterface;
    MUTEX                       Mutex;
};

#define XENFILT_PVDEVICE_TAG    'EDVP'

static FORCEINLINE PVOID
__PvdeviceAllocate(
    IN  ULONG   Length
    )
{
    return __AllocatePoolWithTag(NonPagedPool, Length, XENFILT_PVDEVICE_TAG);
}

static FORCEINLINE VOID
__PvdeviceFree(
    IN  PVOID   Buffer
    )
{
    ExFreePoolWithTag(Buffer, XENFILT_PVDEVICE_TAG);
}

static const CHAR *PvdeviceLegacyPrefix[] = {
    "PCI\\VEN_5853&DEV_0001",
    "PCI\\VEN_5853&DEV_0002",
};

static BOOLEAN
PvdeviceIsLegacy(
    IN  PXENFILT_PVDEVICE_CONTEXT   Context,
    IN  PCHAR                       DeviceID
    )
{
    ULONG                           Index;

    UNREFERENCED_PARAMETER(Context);

    for (Index = 0; Index < ARRAYSIZE(PvdeviceLegacyPrefix); Index++) {
        const CHAR  *Prefix = PvdeviceLegacyPrefix[Index];

        if (_strnicmp(DeviceID, Prefix, strlen(Prefix)) == 0)
            return TRUE;
    }

    return FALSE;
}

static const CHAR *PvdeviceVendorDeviceID[] = {
    "PCI\\VEN_5853&DEV_C000&SUBSYS_C0005853&REV_01", // XenServer
};

static BOOLEAN
PvdeviceIsVendorPresent(
    IN  PXENFILT_PVDEVICE_CONTEXT   Context
    )
{
    ULONG                           Index;

    for (Index = 0; Index < ARRAYSIZE(PvdeviceVendorDeviceID); Index++) {
        const CHAR  *DeviceID = PvdeviceVendorDeviceID[Index];

        if (XENFILT_EMULATED(IsDevicePresent,
                             &Context->EmulatedInterface,
                             (PCHAR)DeviceID,
                             NULL))
            return TRUE;
    }

    return FALSE;
}

static NTSTATUS
PvdeviceGetActive(
    IN  PINTERFACE              Interface,
    OUT PCHAR                   DeviceID,
    OUT PCHAR                   InstanceID
    )
{
    PXENFILT_PVDEVICE_CONTEXT   Context = Interface->Context;
    HANDLE                      ParametersKey;
    PANSI_STRING                Ansi;
    NTSTATUS                    status;

    Trace("====>\n");

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    ParametersKey = DriverGetParametersKey();

    AcquireMutex(&Context->Mutex);

    status = RegistryQuerySzValue(ParametersKey,
                                  "ActiveDeviceID",
                                  &Ansi);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = RtlStringCbPrintfA(DeviceID,
                                MAX_DEVICE_ID_LEN,
                                "%Z",
                                &Ansi[0]);
    ASSERT(NT_SUCCESS(status));

    RegistryFreeSzValue(Ansi);

    status = RegistryQuerySzValue(ParametersKey,
                                  "ActiveInstanceID",
                                  &Ansi);
    if (!NT_SUCCESS(status))
        goto fail2;

    status = RtlStringCbPrintfA(InstanceID,
                                MAX_DEVICE_ID_LEN,
                                "%Z",
                                &Ansi[0]);
    ASSERT(NT_SUCCESS(status));

    RegistryFreeSzValue(Ansi);

    ReleaseMutex(&Context->Mutex);

    Trace("<====\n");

    return STATUS_SUCCESS;

fail2:
fail1:
    ReleaseMutex(&Context->Mutex);

    return status;
}

static NTSTATUS
PvdeviceSetActive(
    IN  PINTERFACE              Interface,
    IN  PCHAR                   DeviceID,
    IN  PCHAR                   InstanceID
    )
{
    PXENFILT_PVDEVICE_CONTEXT   Context = Interface->Context;
    HANDLE                      ParametersKey;
    ANSI_STRING                 Ansi[2];
    NTSTATUS                    status;

    Trace("====>\n");

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    ParametersKey = DriverGetParametersKey();

    AcquireMutex(&Context->Mutex);

    status = STATUS_UNSUCCESSFUL;
    if (PvdeviceIsLegacy(Context, DeviceID) &&
        PvdeviceIsVendorPresent(Context))
        goto fail1;

    RtlZeroMemory(Ansi, sizeof (ANSI_STRING) * 2);

    RtlInitAnsiString(&Ansi[0], DeviceID);

    status = RegistryUpdateSzValue(ParametersKey,
                                   "ActiveDeviceID",
                                   Ansi);
    if (!NT_SUCCESS(status))
        goto fail2;

    RtlInitAnsiString(&Ansi[0], InstanceID);

    status = RegistryUpdateSzValue(ParametersKey,
                                   "ActiveInstanceID",
                                   Ansi);
    if (!NT_SUCCESS(status))
        goto fail3;

    Info("%s\\%s\n", DeviceID, InstanceID);

    ReleaseMutex(&Context->Mutex);

    Trace("<====\n");

    return STATUS_SUCCESS;

fail3:
fail2:
fail1:
    ReleaseMutex(&Context->Mutex);

    return status;
}

static NTSTATUS
PvdeviceClearActive(
    IN  PINTERFACE              Interface
    )
{
    PXENFILT_PVDEVICE_CONTEXT   Context = Interface->Context;
    HANDLE                      ParametersKey;
    NTSTATUS                    status;

    Trace("====>\n");

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    ParametersKey = DriverGetParametersKey();

    AcquireMutex(&Context->Mutex);

    status = RegistryDeleteValue(ParametersKey,
                                 "ActiveDeviceID");
    if (!NT_SUCCESS(status))
        goto fail1;

    status = RegistryDeleteValue(ParametersKey,
                                 "ActiveInstanceID");
    if (!NT_SUCCESS(status))
        goto fail2;

    Info("DONE\n");

    ReleaseMutex(&Context->Mutex);

    Trace("<====\n");

    return STATUS_SUCCESS;

fail2:
fail1:
    ReleaseMutex(&Context->Mutex);

    return status;
}

static NTSTATUS
PvdeviceAcquire(
    IN  PINTERFACE              Interface
    )
{
    PXENFILT_PVDEVICE_CONTEXT   Context = Interface->Context;
    KIRQL                       Irql;
    NTSTATUS                    status;

    KeAcquireSpinLock(&Context->Lock, &Irql);

    if (Context->References++ != 0)
        goto done;

    Trace("====>\n");

    status = XENFILT_EMULATED(Acquire, &Context->EmulatedInterface);
    if (!NT_SUCCESS(status))
        goto fail1;

    Trace("<====\n");

done:
    KeReleaseSpinLock(&Context->Lock, Irql);

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    KeReleaseSpinLock(&Context->Lock, Irql);

    return status;
}

static VOID
PvdeviceRelease(
    IN  PINTERFACE              Interface
    )
{
    PXENFILT_PVDEVICE_CONTEXT   Context = Interface->Context;
    KIRQL                       Irql;

    KeAcquireSpinLock(&Context->Lock, &Irql);

    if (--Context->References > 0)
        goto done;

    Trace("====>\n");

    XENFILT_EMULATED(Release, &Context->EmulatedInterface);

    Trace("<====\n");

done:
    KeReleaseSpinLock(&Context->Lock, Irql);
}

static struct _XENFILT_PVDEVICE_INTERFACE_V1 PvdeviceInterfaceVersion1 = {
    { sizeof (struct _XENFILT_PVDEVICE_INTERFACE_V1), 1, NULL, NULL, NULL },
    PvdeviceAcquire,
    PvdeviceRelease,
    PvdeviceGetActive,
    PvdeviceSetActive,
    PvdeviceClearActive
};

NTSTATUS
PvdeviceInitialize(
    OUT PXENFILT_PVDEVICE_CONTEXT   *Context
    )
{
    NTSTATUS                        status;

    Trace("====>\n");

    *Context = __PvdeviceAllocate(sizeof (XENFILT_PVDEVICE_CONTEXT));

    status = STATUS_NO_MEMORY;
    if (*Context == NULL)
        goto fail1;

    status = EmulatedGetInterface(DriverGetEmulatedContext(),
                                  XENFILT_EMULATED_INTERFACE_VERSION_MAX,
                                  (PINTERFACE)&(*Context)->EmulatedInterface,
                                  sizeof ((*Context)->EmulatedInterface));
    ASSERT(NT_SUCCESS(status));

    KeInitializeSpinLock(&(*Context)->Lock);
    InitializeMutex(&(*Context)->Mutex);

    Trace("<====\n");

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

NTSTATUS
PvdeviceGetInterface(
    IN      PXENFILT_PVDEVICE_CONTEXT   Context,
    IN      ULONG                       Version,
    IN OUT  PINTERFACE                  Interface,
    IN      ULONG                       Size
    )
{
    NTSTATUS                            status;

    ASSERT(Context != NULL);

    switch (Version) {
    case 1: {
        struct _XENFILT_PVDEVICE_INTERFACE_V1   *PvdeviceInterface;

        PvdeviceInterface = (struct _XENFILT_PVDEVICE_INTERFACE_V1 *)Interface;

        status = STATUS_BUFFER_OVERFLOW;
        if (Size < sizeof (struct _XENFILT_PVDEVICE_INTERFACE_V1))
            break;

        *PvdeviceInterface = PvdeviceInterfaceVersion1;

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
PvdeviceTeardown(
    IN  PXENFILT_PVDEVICE_CONTEXT   Context
    )
{
    Trace("====>\n");

    RtlZeroMemory(&Context->Mutex, sizeof (MUTEX));
    RtlZeroMemory(&Context->Lock, sizeof (KSPIN_LOCK));

    RtlZeroMemory(&Context->EmulatedInterface,
                  sizeof (XENFILT_EMULATED_INTERFACE));

    ASSERT(IsZeroMemory(Context, sizeof (XENFILT_PVDEVICE_CONTEXT)));
    __PvdeviceFree(Context);

    Trace("<====\n");
}
