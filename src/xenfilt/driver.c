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
#include <xen.h>
#include <util.h>

#include "fdo.h"
#include "pdo.h"
#include "driver.h"
#include "emulated.h"
#include "unplug.h"
#include "registry.h"
#include "mutex.h"
#include "dbg_print.h"
#include "assert.h"
#include "version.h"

extern PULONG       InitSafeBootMode;

typedef struct _XENFILT_DRIVER {
    PDRIVER_OBJECT              DriverObject;
    HANDLE                      ParametersKey;
    HANDLE                      UnplugKey;

    PCHAR                       ActiveDeviceID;
    PCHAR                       ActiveInstanceID;

    PXENFILT_EMULATED_CONTEXT   EmulatedContext;
    PXENFILT_UNPLUG_CONTEXT     UnplugContext;

    XENFILT_EMULATED_INTERFACE  EmulatedInterface;
    XENFILT_UNPLUG_INTERFACE    UnplugInterface;
} XENFILT_DRIVER, *PXENFILT_DRIVER;

static XENFILT_DRIVER   Driver;

#define XENFILT_DRIVER_TAG  'VIRD'

static FORCEINLINE PVOID
__DriverAllocate(
    IN  ULONG   Length
    )
{
    return __AllocatePoolWithTag(NonPagedPool, Length, XENFILT_DRIVER_TAG);
}

static FORCEINLINE VOID
__DriverFree(
    IN  PVOID   Buffer
    )
{
    ExFreePoolWithTag(Buffer, XENFILT_DRIVER_TAG);
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
__DriverSetUnplugKey(
    IN  HANDLE  Key
    )
{
    Driver.UnplugKey = Key;
}

static FORCEINLINE HANDLE
__DriverGetUnplugKey(
    VOID
    )
{
    return Driver.UnplugKey;
}

HANDLE
DriverGetUnplugKey(
    VOID
    )
{
    return __DriverGetUnplugKey();
}

#define DEFINE_DRIVER_GET_CONTEXT(_Interface, _Type)            \
static FORCEINLINE _Type                                        \
__DriverGet ## _Interface ## Context(                           \
    VOID                                                        \
    )                                                           \
{                                                               \
    return Driver. ## _Interface ## Context;                    \
}                                                               \
                                                                \
_Type                                                           \
DriverGet ## _Interface ## Context(                             \
    VOID                                                        \
    )                                                           \
{                                                               \
    return __DriverGet ## _Interface ## Context();              \
}

DEFINE_DRIVER_GET_CONTEXT(Emulated, PXENFILT_EMULATED_CONTEXT)
DEFINE_DRIVER_GET_CONTEXT(Unplug, PXENFILT_UNPLUG_CONTEXT)

#define SERVICES_KEY L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Services"

#define SERVICE_KEY(_Driver)    \
        SERVICES_KEY L"\\" L#_Driver

#define PARAMETERS_KEY(_Driver) \
        SERVICE_KEY(_Driver) L"\\Parameters"

static NTSTATUS
DriverSetActiveDeviceInstance(
    VOID
    )
{
    UNICODE_STRING  Unicode;
    HANDLE          ParametersKey;
    PANSI_STRING    Ansi;
    NTSTATUS        status;

    RtlInitUnicodeString(&Unicode, PARAMETERS_KEY(XENBUS));
    
    status = RegistryOpenKey(NULL, &Unicode, KEY_READ, &ParametersKey);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = RegistryQuerySzValue(ParametersKey,
                                  "ActiveDeviceID",
                                  &Ansi);
    if (!NT_SUCCESS(status)) {
        if (status != STATUS_OBJECT_NAME_NOT_FOUND)
            goto fail2;

        // The active device is not yet set
        goto done;
    }

    Driver.ActiveDeviceID = __DriverAllocate(Ansi[0].MaximumLength);

    status = STATUS_NO_MEMORY;
    if (Driver.ActiveDeviceID == NULL)
        goto fail3;

    RtlCopyMemory(Driver.ActiveDeviceID, Ansi[0].Buffer, Ansi[0].Length);

    RegistryFreeSzValue(Ansi);
        
    status = RegistryQuerySzValue(ParametersKey,
                                  "ActiveInstanceID",
                                  &Ansi);
    if (!NT_SUCCESS(status))
        goto fail4;

    Driver.ActiveInstanceID = __DriverAllocate(Ansi[0].MaximumLength);

    status = STATUS_NO_MEMORY;
    if (Driver.ActiveInstanceID == NULL)
        goto fail5;

    RtlCopyMemory(Driver.ActiveInstanceID, Ansi[0].Buffer, Ansi[0].Length);

    RegistryFreeSzValue(Ansi);

done:
    if (Driver.ActiveDeviceID != NULL)
        Info("%s/%s\n", Driver.ActiveDeviceID, Driver.ActiveInstanceID);

    RegistryCloseKey(ParametersKey);

    return STATUS_SUCCESS;

fail5:
    Error("fail5\n");

    RegistryFreeSzValue(Ansi);

fail4:
    Error("fail4\n");

    __DriverFree(Driver.ActiveDeviceID);
    Driver.ActiveDeviceID = NULL;

    goto fail2;

fail3:
    Error("fail3\n");

    RegistryFreeSzValue(Ansi);

fail2:
    Error("fail2\n");

    RegistryCloseKey(ParametersKey);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

extern const CHAR *
DriverGetActiveDeviceID(
    VOID
    )
{
    return Driver.ActiveDeviceID;
}

extern const CHAR *
DriverGetActiveInstanceID(
    VOID
    )
{
    return Driver.ActiveInstanceID;
}

DRIVER_UNLOAD   DriverUnload;

VOID
DriverUnload(
    IN  PDRIVER_OBJECT  DriverObject
    )
{
    HANDLE              ParametersKey;
    HANDLE              UnplugKey;

    ASSERT3P(DriverObject, ==, __DriverGetDriverObject());

    Trace("====>\n");

    if (*InitSafeBootMode > 0)
        goto done;

    XENFILT_UNPLUG(Release, &Driver.UnplugInterface);

    XENFILT_EMULATED(Release, &Driver.EmulatedInterface);

    RtlZeroMemory(&Driver.UnplugInterface,
                  sizeof (XENFILT_UNPLUG_INTERFACE));

    RtlZeroMemory(&Driver.EmulatedInterface,
                  sizeof (XENFILT_EMULATED_INTERFACE));

    UnplugTeardown(Driver.UnplugContext);
    Driver.UnplugContext = NULL;

    EmulatedTeardown(Driver.EmulatedContext);
    Driver.EmulatedContext = NULL;

    if (Driver.ActiveDeviceID != NULL) {
        __DriverFree(Driver.ActiveDeviceID);
        Driver.ActiveDeviceID = NULL;

        __DriverFree(Driver.ActiveInstanceID);
        Driver.ActiveInstanceID = NULL;
    }

    UnplugKey = __DriverGetUnplugKey();
    __DriverSetUnplugKey(NULL);
    RegistryCloseKey(UnplugKey);

    ParametersKey = __DriverGetParametersKey();
    __DriverSetParametersKey(NULL);
    RegistryCloseKey(ParametersKey);

    RegistryTeardown();

    Info("XENFILT %d.%d.%d (%d) (%02d.%02d.%04d)\n",
         MAJOR_VERSION,
         MINOR_VERSION,
         MICRO_VERSION,
         BUILD_NUMBER,
         DAY,
         MONTH,
         YEAR);

done:
    __DriverSetDriverObject(NULL);

    ASSERT(IsZeroMemory(&Driver, sizeof (XENFILT_DRIVER)));

    Trace("<====\n");
}

__drv_functionClass(IO_COMPLETION_ROUTINE)
__drv_sameIRQL
static NTSTATUS
DriverQueryIdCompletion(
    IN  PDEVICE_OBJECT  DeviceObject,
    IN  PIRP            Irp,
    IN  PVOID           Context
    )
{
    PKEVENT             Event = Context;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    KeSetEvent(Event, IO_NO_INCREMENT, FALSE);

    return STATUS_MORE_PROCESSING_REQUIRED;
}

static FORCEINLINE NTSTATUS
DriverQueryId(
    IN  PDEVICE_OBJECT      PhysicalDeviceObject,
    IN  BUS_QUERY_ID_TYPE   IdType,
    OUT PVOID               *Information
    )
{
    PDEVICE_OBJECT          DeviceObject;
    PIRP                    Irp;
    KEVENT                  Event;
    PIO_STACK_LOCATION      StackLocation;
    NTSTATUS                status;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    DeviceObject = IoGetAttachedDeviceReference(PhysicalDeviceObject);

    Irp = IoAllocateIrp(DeviceObject->StackSize, FALSE);

    status = STATUS_INSUFFICIENT_RESOURCES;
    if (Irp == NULL)
        goto fail1;

    StackLocation = IoGetNextIrpStackLocation(Irp);

    StackLocation->MajorFunction = IRP_MJ_PNP;
    StackLocation->MinorFunction = IRP_MN_QUERY_ID;
    StackLocation->Flags = 0;
    StackLocation->Parameters.QueryId.IdType = IdType;
    StackLocation->DeviceObject = DeviceObject;
    StackLocation->FileObject = NULL;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    IoSetCompletionRoutine(Irp,
                           DriverQueryIdCompletion,
                           &Event,
                           TRUE,
                           TRUE,
                           TRUE);

    // Default completion status
    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;

    status = IoCallDriver(DeviceObject, Irp);
    if (status == STATUS_PENDING) {
        (VOID) KeWaitForSingleObject(&Event,
                                     Executive,
                                     KernelMode,
                                     FALSE,
                                     NULL);
        status = Irp->IoStatus.Status;
    } else {
        ASSERT3U(status, ==, Irp->IoStatus.Status);
    }

    if (!NT_SUCCESS(status))
        goto fail2;

    *Information = (PVOID)Irp->IoStatus.Information;

    IoFreeIrp(Irp);
    ObDereferenceObject(DeviceObject);

    return STATUS_SUCCESS;

fail2:
    IoFreeIrp(Irp);

fail1:
    ObDereferenceObject(DeviceObject);

    return status;
}

static XENFILT_EMULATED_OBJECT_TYPE
DriverGetEmulatedType(
    IN  PANSI_STRING                Ansi
    )
{
    XENFILT_EMULATED_OBJECT_TYPE    Type;

    if (_strnicmp(Ansi->Buffer, "DEVICE", Ansi->Length) == 0)
        Type = XENFILT_EMULATED_OBJECT_TYPE_DEVICE;
    else if (_strnicmp(Ansi->Buffer, "DISK", Ansi->Length) == 0)
        Type = XENFILT_EMULATED_OBJECT_TYPE_DISK;
    else
        Type = XENFILT_EMULATED_OBJECT_TYPE_INVALID;

    return Type;
}

DRIVER_ADD_DEVICE   DriverAddDevice;

NTSTATUS
#pragma prefast(suppress:28152) // Does not clear DO_DEVICE_INITIALIZING
DriverAddDevice(
    IN  PDRIVER_OBJECT  DriverObject,
    IN  PDEVICE_OBJECT  PhysicalDeviceObject
    )
{
    HANDLE              ParametersKey;
    PWCHAR              DeviceID;
    PWCHAR              InstanceID;
    UNICODE_STRING      Unicode;
    ANSI_STRING         Name;
    PANSI_STRING        Type;
    NTSTATUS            status;

    ASSERT3P(DriverObject, ==, __DriverGetDriverObject());

    ParametersKey = __DriverGetParametersKey();

    status = DriverQueryId(PhysicalDeviceObject, BusQueryDeviceID, &DeviceID);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = DriverQueryId(PhysicalDeviceObject, BusQueryInstanceID, &InstanceID);
    if (!NT_SUCCESS(status))
        goto fail2;

    RtlInitUnicodeString(&Unicode, DeviceID);

    status = RtlUnicodeStringToAnsiString(&Name, &Unicode, TRUE);
    if (!NT_SUCCESS(status))
        goto fail3;

    status = RegistryQuerySzValue(ParametersKey,
                                  Name.Buffer,
                                  &Type);
    if (NT_SUCCESS(status)) {
        status = FdoCreate(PhysicalDeviceObject,
                           DeviceID,
                           InstanceID,
                           DriverGetEmulatedType(Type));

        if (!NT_SUCCESS(status))
            goto fail4;

        RegistryFreeSzValue(Type);
    }

    RtlFreeAnsiString(&Name);
    ExFreePool(InstanceID);
    ExFreePool(DeviceID);

    return STATUS_SUCCESS;

fail4:
    RegistryFreeSzValue(Type);

    RtlFreeAnsiString(&Name);

fail3:
    ExFreePool(InstanceID);

fail2:
    ExFreePool(DeviceID);

fail1:
    return status;
}

DRIVER_DISPATCH DriverDispatch;

NTSTATUS 
DriverDispatch(
    IN PDEVICE_OBJECT   DeviceObject,
    IN PIRP             Irp
    )
{
    PXENFILT_DX         Dx;
    NTSTATUS            status;

    Dx = (PXENFILT_DX)DeviceObject->DeviceExtension;
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
        PXENFILT_PDO    Pdo = Dx->Pdo;

        status = PdoDispatch(Pdo, Irp);
        break;
    }
    case FUNCTION_DEVICE_OBJECT: {
        PXENFILT_FDO    Fdo = Dx->Fdo;

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
    HANDLE              UnplugKey;
    ULONG               Index;
    NTSTATUS            status;

    ASSERT3P(__DriverGetDriverObject(), ==, NULL);

    ExInitializeDriverRuntime(DrvRtPoolNxOptIn);

    __DbgPrintEnable();

    Trace("====>\n");

    __DriverSetDriverObject(DriverObject);

    DriverObject->DriverUnload = DriverUnload;

    if (*InitSafeBootMode > 0)
        goto done;

    XenTouch();

    Info("XENFILT %d.%d.%d (%d) (%02d.%02d.%04d)\n",
         MAJOR_VERSION,
         MINOR_VERSION,
         MICRO_VERSION,
         BUILD_NUMBER,
         DAY,
         MONTH,
         YEAR);

    status = RegistryInitialize(RegistryPath);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = RegistryOpenServiceKey(KEY_READ, &ServiceKey);
    if (!NT_SUCCESS(status))
        goto fail2;

    status = RegistryOpenSubKey(ServiceKey, "Parameters", KEY_READ, &ParametersKey);
    if (!NT_SUCCESS(status))
        goto fail3;

    __DriverSetParametersKey(ParametersKey);

    status = RegistryOpenSubKey(ServiceKey, "Unplug", KEY_READ, &UnplugKey);
    if (!NT_SUCCESS(status))
        goto fail4;

    __DriverSetUnplugKey(UnplugKey);

    status = DriverSetActiveDeviceInstance();
    if (!NT_SUCCESS(status))
        goto fail5;

    status = EmulatedInitialize(&Driver.EmulatedContext);
    if (!NT_SUCCESS(status))
        goto fail6;

    status = UnplugInitialize(&Driver.UnplugContext);
    if (!NT_SUCCESS(status))
        goto fail7;

    status = EmulatedGetInterface(__DriverGetEmulatedContext(),
                                  XENFILT_EMULATED_INTERFACE_VERSION_MAX,
                                  (PINTERFACE)&Driver.EmulatedInterface,
                                  sizeof (Driver.EmulatedInterface));
    ASSERT(NT_SUCCESS(status));
    ASSERT(Driver.EmulatedInterface.Interface.Context != NULL);

    status = UnplugGetInterface(__DriverGetUnplugContext(),
                                 XENFILT_UNPLUG_INTERFACE_VERSION_MAX,
                                 (PINTERFACE)&Driver.UnplugInterface,
                                 sizeof (Driver.UnplugInterface));
    ASSERT(NT_SUCCESS(status));
    ASSERT(Driver.UnplugInterface.Interface.Context != NULL);

    status = XENFILT_EMULATED(Acquire, &Driver.EmulatedInterface);
    if (!NT_SUCCESS(status))
        goto fail8;

    status = XENFILT_UNPLUG(Acquire, &Driver.UnplugInterface);
    if (!NT_SUCCESS(status))
        goto fail9;

    RegistryCloseKey(ServiceKey);

    DriverObject->DriverExtension->AddDevice = DriverAddDevice;

    for (Index = 0; Index <= IRP_MJ_MAXIMUM_FUNCTION; Index++) {
#pragma prefast(suppress:28169) // No __drv_dispatchType annotation
#pragma prefast(suppress:28168) // No matching __drv_dispatchType annotation for IRP_MJ_CREATE
        DriverObject->MajorFunction[Index] = DriverDispatch;
    }

done:
    Trace("<====\n");
    return STATUS_SUCCESS;

fail9:
    Error("fail9\n");

    XENFILT_EMULATED(Release, &Driver.EmulatedInterface);

fail8:
    Error("fail8\n");

    RtlZeroMemory(&Driver.UnplugInterface,
                  sizeof (XENFILT_UNPLUG_INTERFACE));

    RtlZeroMemory(&Driver.EmulatedInterface,
                  sizeof (XENFILT_EMULATED_INTERFACE));

    UnplugTeardown(Driver.UnplugContext);
    Driver.UnplugContext = NULL;

fail7:
    Error("fail7\n");

    EmulatedTeardown(Driver.EmulatedContext);
    Driver.EmulatedContext = NULL;

fail6:
    Error("fail6\n");

    if (Driver.ActiveDeviceID != NULL) {
        __DriverFree(Driver.ActiveDeviceID);
        Driver.ActiveDeviceID = NULL;

        __DriverFree(Driver.ActiveInstanceID);
        Driver.ActiveInstanceID = NULL;
    }

fail5:
    Error("fail5\n");

    __DriverSetUnplugKey(NULL);
    RegistryCloseKey(UnplugKey);

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

    ASSERT(IsZeroMemory(&Driver, sizeof (XENFILT_DRIVER)));

    return status;
}
