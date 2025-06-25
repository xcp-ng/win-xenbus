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

#define XEN_API __declspec(dllexport)

#define INITGUID 1

#include <ntddk.h>
#include <ntstrsafe.h>
#include <devguid.h>
#include <xen.h>

#include "registry.h"
#include "driver.h"
#include "dbg_print.h"
#include "assert.h"
#include "util.h"

#define MAXNAMELEN  128

//
// The canonical location for active device information is the XENFILT
// Parameters key.
//
#define ACTIVE_PATH "\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Services\\XENFILT\\Parameters"

XEN_API
NTSTATUS
ConfigGetActive(
    _In_ PCSTR              Key,
    _Outptr_result_z_ PSTR  *Value
    )
{
    HANDLE                  ActiveKey;
    CHAR                    Name[MAXNAMELEN];
    PANSI_STRING            Ansi;
    ULONG                   Length;
    NTSTATUS                status;

    Trace("====>\n");

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    status = RegistryOpenSubKey(NULL,
                                ACTIVE_PATH,
                                KEY_READ,
                                &ActiveKey);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = RtlStringCbPrintfA(Name, MAXNAMELEN, "Active%s", Key);
    ASSERT(NT_SUCCESS(status));

    status = RegistryQuerySzValue(ActiveKey,
                                  Name,
                                  NULL,
                                  &Ansi);
    if (!NT_SUCCESS(status))
        goto fail2;

    Length = Ansi[0].Length + sizeof (CHAR);
    *Value = __AllocatePoolWithTag(PagedPool, Length, 'SUB');

    status = STATUS_NO_MEMORY;
    if (*Value == NULL)
        goto fail3;

    status = RtlStringCbPrintfA(*Value,
                                Length,
                                "%Z",
                                &Ansi[0]);
    ASSERT(NT_SUCCESS(status));

    RegistryFreeSzValue(Ansi);

    RegistryCloseKey(ActiveKey);

    Trace("<====\n");

    return STATUS_SUCCESS;

fail3:
    Error("fail3\n");

fail2:
    if (status != STATUS_OBJECT_NAME_NOT_FOUND)
        Error("fail2\n");

    RegistryCloseKey(ActiveKey);

fail1:
    if (status != STATUS_OBJECT_NAME_NOT_FOUND)
        Error("fail1 (%08x)\n", status);

    return status;
}

static FORCEINLINE BOOLEAN
__ConfigIsDeviceLegacy(
    _In_ PSTR   DeviceID
    )
{
    UNREFERENCED_PARAMETER(DeviceID);

#ifdef VENDOR_DEVICE_ID_STR
    PCSTR       VendorDeviceID = "PCI\\VEN_5853&DEV_" VENDOR_DEVICE_ID_STR;

    return _strnicmp(DeviceID, VendorDeviceID, strlen(VendorDeviceID)) != 0;
#endif

    return TRUE;
}

#define ENUM_PATH "\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Enum"

static FORCEINLINE BOOLEAN
__ConfigIsVendorDevicePresent(
    VOID
    )
{
#ifdef VENDOR_DEVICE_ID_STR
    HANDLE      EnumKey;
    HANDLE      DeviceKey;
    BOOLEAN     Found;
    NTSTATUS    status;
    PCSTR       VendorDeviceID = "PCI\\VEN_5853&DEV_" VENDOR_DEVICE_ID_STR "&SUBSYS_C0005853&REV_01";

    status = RegistryOpenSubKey(NULL,
                                ENUM_PATH,
                                KEY_READ,
                                &EnumKey);
    if (!NT_SUCCESS(status))
        goto fail1;

    Found = FALSE;

    status = RegistryOpenSubKey(EnumKey,
                                (PSTR)VendorDeviceID,
                                KEY_READ,
                                &DeviceKey);
    if (!NT_SUCCESS(status))
        goto done;

    RegistryCloseKey(DeviceKey);
    Found = TRUE;

done:
    RegistryCloseKey(EnumKey);

    return Found;

fail1:
    Error("fail1 (%08x)\n", status);

#endif
    return FALSE;
}

XEN_API
NTSTATUS
ConfigSetActive(
    _In_ PSTR   DeviceID,
    _In_ PSTR   InstanceID,
    _In_ PSTR   LocationInformation
    )
{
    HANDLE      ActiveKey;
    ANSI_STRING Ansi[2];
    NTSTATUS    status;

    Trace("====>\n");

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    status = RegistryOpenSubKey(NULL,
                                ACTIVE_PATH,
                                KEY_ALL_ACCESS,
                                &ActiveKey);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = STATUS_UNSUCCESSFUL;
    if (__ConfigIsDeviceLegacy(DeviceID) &&
        __ConfigIsVendorDevicePresent())
        goto fail2;

    RtlZeroMemory(Ansi, sizeof (ANSI_STRING) * 2);

    RtlInitAnsiString(&Ansi[0], DeviceID);

    status = RegistryUpdateSzValue(ActiveKey,
                                   "ActiveDeviceID",
                                   REG_SZ,
                                   Ansi);
    if (!NT_SUCCESS(status))
        goto fail3;

    RtlInitAnsiString(&Ansi[0], InstanceID);

    status = RegistryUpdateSzValue(ActiveKey,
                                   "ActiveInstanceID",
                                   REG_SZ,
                                   Ansi);
    if (!NT_SUCCESS(status))
        goto fail4;

    RtlInitAnsiString(&Ansi[0], LocationInformation);

    status = RegistryUpdateSzValue(ActiveKey,
                                   "ActiveLocationInformation",
                                   REG_SZ,
                                   Ansi);
    if (!NT_SUCCESS(status))
        goto fail5;

    Info("%s\\%s: %s\n", DeviceID, InstanceID, LocationInformation);

    RegistryCloseKey(ActiveKey);

    Trace("<====\n");

    return STATUS_SUCCESS;

fail5:
    Error("fail5\n");

fail4:
    Error("fail4\n");

fail3:
    Error("fail3\n");

fail2:
    Error("fail2\n");

    RegistryCloseKey(ActiveKey);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

XEN_API
NTSTATUS
ConfigUpdateActive(
    _In_ PSTR   DeviceID,
    _In_ PSTR   InstanceID,
    _In_ PSTR   LocationInformation
    )
{
    HANDLE      ActiveKey;
    ANSI_STRING Ansi[2];
    PSTR        ActiveInstanceID;
    PSTR        ActiveLocationInformation;
    NTSTATUS    status;

    Trace("====>\n");

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    status = RegistryOpenSubKey(NULL,
                                ACTIVE_PATH,
                                KEY_ALL_ACCESS,
                                &ActiveKey);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = STATUS_UNSUCCESSFUL;
    if (__ConfigIsDeviceLegacy(DeviceID) &&
        __ConfigIsVendorDevicePresent())
        goto fail2;

    RtlZeroMemory(Ansi, sizeof (ANSI_STRING) * 2);

    status = ConfigGetActive("InstanceID", &ActiveInstanceID);
    if (NT_SUCCESS(status)) {
        ExFreePool(ActiveInstanceID);
    } else {
        RtlInitAnsiString(&Ansi[0], InstanceID);

        status = RegistryUpdateSzValue(ActiveKey,
                                       "ActiveInstanceID",
                                       REG_SZ,
                                       Ansi);
        if (!NT_SUCCESS(status))
            goto fail3;
    }

    status = ConfigGetActive("LocationInformation", &ActiveLocationInformation);
    if (NT_SUCCESS(status)) {
        ExFreePool(ActiveLocationInformation);
    } else {
        RtlInitAnsiString(&Ansi[0], LocationInformation);

        status = RegistryUpdateSzValue(ActiveKey,
                                       "ActiveLocationInformation",
                                       REG_SZ,
                                       Ansi);
        if (!NT_SUCCESS(status))
            goto fail4;
    }

    Info("%s\\%s: %s\n", DeviceID, InstanceID, LocationInformation);

    RegistryCloseKey(ActiveKey);

    Trace("<====\n");

    return STATUS_SUCCESS;

fail4:
    Error("fail4\n");

fail3:
    Error("fail3\n");

fail2:
    Error("fail2\n");

    RegistryCloseKey(ActiveKey);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

XEN_API
NTSTATUS
ConfigClearActive(
    VOID
    )
{
    HANDLE      ActiveKey;
    NTSTATUS    status;

    Trace("====>\n");

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    status = RegistryOpenSubKey(NULL,
                                ACTIVE_PATH,
                                KEY_ALL_ACCESS,
                                &ActiveKey);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = RegistryDeleteValue(ActiveKey,
                                 "ActiveDeviceID");
    if (!NT_SUCCESS(status))
        goto fail2;

    status = RegistryDeleteValue(ActiveKey,
                                 "ActiveInstanceID");
    if (!NT_SUCCESS(status))
        goto fail3;

    Info("DONE\n");

    RegistryCloseKey(ActiveKey);

    Trace("<====\n");

    return STATUS_SUCCESS;

fail3:
    Error("fail3\n");

fail2:
    Error("fail2\n");

    RegistryCloseKey(ActiveKey);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

XEN_API
NTSTATUS
ConfigRequestReboot(
    _In_ HANDLE     ParametersKey,
    _In_ PSTR       Module
    )
{
    PANSI_STRING    Ansi;
    CHAR            RequestKeyName[MAXNAMELEN];
    HANDLE          RequestKey;
    HANDLE          SubKey;
    NTSTATUS        status;

    Info("====>\n");

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    status = RegistryQuerySzValue(ParametersKey,
                                  "RequestKey",
                                  NULL,
                                  &Ansi);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = RtlStringCbPrintfA(RequestKeyName,
                                MAXNAMELEN,
                                "\\Registry\\Machine\\%Z",
                                &Ansi[0]);
    ASSERT(NT_SUCCESS(status));

    status = RegistryCreateSubKey(NULL,
                                  RequestKeyName,
                                  REG_OPTION_NON_VOLATILE,
                                  &RequestKey);
    if (!NT_SUCCESS(status))
        goto fail2;

    status = RegistryCreateSubKey(RequestKey,
                                  Module,
                                  REG_OPTION_VOLATILE,
                                  &SubKey);
    if (!NT_SUCCESS(status))
        goto fail3;

    status = RegistryUpdateDwordValue(SubKey,
                                      "Reboot",
                                      1);
    if (!NT_SUCCESS(status))
        goto fail4;

    RegistryCloseKey(SubKey);

    RegistryFreeSzValue(Ansi);

    Info("<====\n");

    return STATUS_SUCCESS;

fail4:
    Error("fail4\n");

    RegistryCloseKey(SubKey);

fail3:
    Error("fail3\n");

    RegistryCloseKey(RequestKey);

fail2:
    Error("fail2\n");

    RegistryFreeSzValue(Ansi);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

XEN_API
NTSTATUS
ConfigQuerySystemStartOption(
    _In_ PSTR           Key,
    _Out_ PANSI_STRING  *Option
    )
{
    return RegistryQuerySystemStartOption(Key, Option);
}
