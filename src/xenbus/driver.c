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
#include <ntstrsafe.h>

#include "registry.h"
#include "fdo.h"
#include "pdo.h"
#include "driver.h"
#include "names.h"
#include "mutex.h"
#include "dbg_print.h"
#include "assert.h"
#include "util.h"
#include "version.h"

extern PULONG       InitSafeBootMode;

typedef struct _XENBUS_DRIVER {
    PDRIVER_OBJECT      DriverObject;
    HANDLE              ParametersKey;
    HANDLE              StatusKey;

    MUTEX               Mutex;
    LIST_ENTRY          List;
    ULONG               References;
} XENBUS_DRIVER, *PXENBUS_DRIVER;

static XENBUS_DRIVER    Driver;

#define XENBUS_DRIVER_TAG   'VIRD'

static FORCEINLINE PVOID
__DriverAllocate(
    IN  ULONG   Length
    )
{
    return __AllocatePoolWithTag(NonPagedPool, Length, XENBUS_DRIVER_TAG);
}

static FORCEINLINE VOID
__DriverFree(
    IN  PVOID   Buffer
    )
{
    ExFreePoolWithTag(Buffer, XENBUS_DRIVER_TAG);
}

static FORCEINLINE VOID
__DriverSetDriverObject(
    IN  PDRIVER_OBJECT  DriverObject
    )
{
    Driver.DriverObject = DriverObject;
}

static FORCEINLINE PDRIVER_OBJECT
__DriverGetDriverObject(
    VOID
    )
{
    return Driver.DriverObject;
}

PDRIVER_OBJECT
DriverGetDriverObject(
    VOID
    )
{
    return __DriverGetDriverObject();
}

static FORCEINLINE VOID
__DriverSetParametersKey(
    IN  HANDLE  Key
    )
{
    Driver.ParametersKey = Key;
}

static FORCEINLINE HANDLE
__DriverGetParametersKey(
    VOID
    )
{
    return Driver.ParametersKey;
}

HANDLE
DriverGetParametersKey(
    VOID
    )
{
    return __DriverGetParametersKey();
}

static FORCEINLINE VOID
__DriverSetStatusKey(
    IN  HANDLE  Key
    )
{
    Driver.StatusKey = Key;
}

static FORCEINLINE HANDLE
__DriverGetStatusKey(
    VOID
    )
{
    return Driver.StatusKey;
}

VOID
DriverRequestReboot(
    VOID
    )
{
    Info("<===>\n");

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    (VOID) RegistryUpdateDwordValue(__DriverGetStatusKey(),
                                    "NeedReboot",
                                    1);
}

static FORCEINLINE VOID
__DriverAcquireMutex(
    VOID
    )
{
    AcquireMutex(&Driver.Mutex);
}

VOID
DriverAcquireMutex(
    VOID
    )
{
    __DriverAcquireMutex();
}

static FORCEINLINE VOID
__DriverReleaseMutex(
    VOID
    )
{
    ReleaseMutex(&Driver.Mutex);
}

VOID
DriverReleaseMutex(
    VOID
    )
{
    __DriverReleaseMutex();
}

VOID
DriverAddFunctionDeviceObject(
    IN  PXENBUS_FDO Fdo
    )
{
    PDEVICE_OBJECT  DeviceObject;
    PXENBUS_DX      Dx;

    DeviceObject = FdoGetDeviceObject(Fdo);
    Dx = (PXENBUS_DX)DeviceObject->DeviceExtension;
    ASSERT3U(Dx->Type, ==, FUNCTION_DEVICE_OBJECT);

    InsertTailList(&Driver.List, &Dx->ListEntry);
    Driver.References++;
}

VOID
DriverRemoveFunctionDeviceObject(
    IN  PXENBUS_FDO Fdo
    )
{
    PDEVICE_OBJECT  DeviceObject;
    PXENBUS_DX      Dx;

    DeviceObject = FdoGetDeviceObject(Fdo);
    Dx = (PXENBUS_DX)DeviceObject->DeviceExtension;
    ASSERT3U(Dx->Type, ==, FUNCTION_DEVICE_OBJECT);

    RemoveEntryList(&Dx->ListEntry);
    ASSERT3U(Driver.References, !=, 0);
    --Driver.References;
}

DRIVER_UNLOAD       DriverUnload;

VOID
DriverUnload(
    IN  PDRIVER_OBJECT  DriverObject
    )
{
    HANDLE              ParametersKey;
    HANDLE              StatusKey;

    ASSERT3P(DriverObject, ==, __DriverGetDriverObject());

    Trace("====>\n");

    if (*InitSafeBootMode > 0)
        goto done;

    ASSERT(IsListEmpty(&Driver.List));
    ASSERT3U(Driver.References, ==, 1);
    --Driver.References;

    RtlZeroMemory(&Driver.List, sizeof (LIST_ENTRY));
    RtlZeroMemory(&Driver.Mutex, sizeof (MUTEX));

    StatusKey = __DriverGetStatusKey();
    __DriverSetStatusKey(NULL);

    RegistryCloseKey(StatusKey);

    ParametersKey = __DriverGetParametersKey();

    RegistryCloseKey(ParametersKey);
    __DriverSetParametersKey(NULL);

    RegistryTeardown();

    Info("XENBUS %d.%d.%d (%d) (%02d.%02d.%04d)\n",
         MAJOR_VERSION,
         MINOR_VERSION,
         MICRO_VERSION,
         BUILD_NUMBER,
         DAY,
         MONTH,
         YEAR);

done:
    __DriverSetDriverObject(NULL);

    ASSERT(IsZeroMemory(&Driver, sizeof (XENBUS_DRIVER)));

    Trace("<====\n");
}

DRIVER_ADD_DEVICE   DriverAddDevice;

NTSTATUS
#pragma prefast(suppress:28152) // Does not clear DO_DEVICE_INITIALIZING
DriverAddDevice(
    IN  PDRIVER_OBJECT  DriverObject,
    IN  PDEVICE_OBJECT  DeviceObject
    )
{
    NTSTATUS            status;

    ASSERT3P(DriverObject, ==, __DriverGetDriverObject());

    Trace("====>\n");

    __DriverAcquireMutex();

    status = FdoCreate(DeviceObject);
    if (!NT_SUCCESS(status))
        goto fail1;

    __DriverReleaseMutex();

    Trace("<====\n");

    return STATUS_SUCCESS;

fail1:
    __DriverReleaseMutex();

    return status;
}

DRIVER_DISPATCH DriverDispatch;

NTSTATUS 
DriverDispatch(
    IN PDEVICE_OBJECT   DeviceObject,
    IN PIRP             Irp
    )
{
    PXENBUS_DX          Dx;
    NTSTATUS            status;

    Dx = (PXENBUS_DX)DeviceObject->DeviceExtension;
    ASSERT3P(Dx->DeviceObject, ==, DeviceObject);

    if (Dx->DevicePnpState == Deleted) {
        status = STATUS_NO_SUCH_DEVICE;

        Irp->IoStatus.Status = status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        goto done;
    }

    status = STATUS_NOT_SUPPORTED;
    switch (Dx->Type) {
    case PHYSICAL_DEVICE_OBJECT: {
        PXENBUS_PDO Pdo = Dx->Pdo;

        status = PdoDispatch(Pdo, Irp);
        break;
    }
    case FUNCTION_DEVICE_OBJECT: {
        PXENBUS_FDO Fdo = Dx->Fdo;

        status = FdoDispatch(Fdo, Irp);
        break;
    }
    default:
        ASSERT(FALSE);
        break;
    }

done:
    return status;
}

DRIVER_INITIALIZE   DriverEntry;

NTSTATUS
DriverEntry(
    IN  PDRIVER_OBJECT  DriverObject,
    IN  PUNICODE_STRING RegistryPath
    )
{
    HANDLE              ServiceKey;
    HANDLE              ParametersKey;
    HANDLE              StatusKey;
    ULONG               Index;
    NTSTATUS            status;

    ASSERT3P(__DriverGetDriverObject(), ==, NULL);

    ExInitializeDriverRuntime(DrvRtPoolNxOptIn);
    WdmlibProcgrpInitialize();

    __DbgPrintEnable();

    Trace("====>\n");

    __DriverSetDriverObject(DriverObject);

    Driver.DriverObject->DriverUnload = DriverUnload;

    if (*InitSafeBootMode > 0)
        goto done;

    Info("%d.%d.%d (%d) (%02d.%02d.%04d)\n",
         MAJOR_VERSION,
         MINOR_VERSION,
         MICRO_VERSION,
         BUILD_NUMBER,
         DAY,
         MONTH,
         YEAR);

    status = XenTouch(__MODULE__,
                      MAJOR_VERSION,
                      MINOR_VERSION,
                      MICRO_VERSION,
                      BUILD_NUMBER);
    if (!NT_SUCCESS(status))
        goto done;

    status = RegistryInitialize(RegistryPath);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = RegistryOpenServiceKey(KEY_READ, &ServiceKey);
    if (!NT_SUCCESS(status))
        goto fail2;

    status = RegistryOpenSubKey(ServiceKey,
                                "Parameters",
                                KEY_READ,
                                &ParametersKey);
    if (!NT_SUCCESS(status))
        goto fail3;

    __DriverSetParametersKey(ParametersKey);

    status = RegistryCreateSubKey(ServiceKey,
                                  "Status",
                                  REG_OPTION_VOLATILE,
                                  &StatusKey);
    if (!NT_SUCCESS(status))
        goto fail4;

    __DriverSetStatusKey(StatusKey);

    RegistryCloseKey(ServiceKey);

    DriverObject->DriverExtension->AddDevice = DriverAddDevice;

    for (Index = 0; Index <= IRP_MJ_MAXIMUM_FUNCTION; Index++) {
#pragma prefast(suppress:28169) // No __drv_dispatchType annotation
#pragma prefast(suppress:28168) // No matching __drv_dispatchType annotation for IRP_MJ_CREATE
       DriverObject->MajorFunction[Index] = DriverDispatch;
    }

    InitializeMutex(&Driver.Mutex);
    InitializeListHead(&Driver.List);
    Driver.References = 1;

done:
    Trace("<====\n");

    return STATUS_SUCCESS;

fail4:
    Error("fail4\n");

    __DriverSetParametersKey(NULL);

    RegistryCloseKey(ParametersKey);

fail3:
    Error("fail3\n");

    RegistryCloseKey(ServiceKey);

fail2:
    Error("fail2\n");

    RegistryTeardown();

fail1:
    Error("fail1 (%08x)\n", status);

    __DriverSetDriverObject(NULL);

    ASSERT(IsZeroMemory(&Driver, sizeof (XENBUS_DRIVER)));

    return status;
}
