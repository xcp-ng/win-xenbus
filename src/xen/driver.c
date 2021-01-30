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

#define XEN_API __declspec(dllexport)

#include <ntddk.h>
#include <procgrp.h>
#include <ntstrsafe.h>
#include <xen.h>

#include "registry.h"
#include "driver.h"
#include "hypercall.h"
#include "log.h"
#include "module.h"
#include "process.h"
#include "system.h"
#include "acpi.h"
#include "unplug.h"
#include "bug_check.h"
#include "dbg_print.h"
#include "assert.h"
#include "util.h"
#include "version.h"

#define DEFAULT_XEN_LOG_LEVEL   LOG_LEVEL_CRITICAL
#define DEFAULT_QEMU_LOG_LEVEL  (LOG_LEVEL_INFO |       \
                                 LOG_LEVEL_WARNING |    \
                                 LOG_LEVEL_ERROR |      \
                                 LOG_LEVEL_CRITICAL)

typedef struct _XEN_DRIVER {
    PLOG_DISPOSITION    XenDisposition;
    PLOG_DISPOSITION    QemuDisposition;
    HANDLE              ParametersKey;
    HANDLE              UnplugKey;
    HANDLE              MemoryKey;
} XEN_DRIVER, *PXEN_DRIVER;

static XEN_DRIVER   Driver;

#define XEN_DRIVER_TAG  'VIRD'

extern PULONG   InitSafeBootMode;

static FORCEINLINE PVOID
__DriverAllocate(
    IN  ULONG   Length
    )
{
    return __AllocatePoolWithTag(NonPagedPool, Length, XEN_DRIVER_TAG);
}

static FORCEINLINE VOID
__DriverFree(
    IN  PVOID   Buffer
    )
{
    __FreePoolWithTag(Buffer, XEN_DRIVER_TAG);
}

static FORCEINLINE BOOLEAN
__DriverSafeMode(
    VOID
    )
{
    return (*InitSafeBootMode > 0) ? TRUE : FALSE;
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

static FORCEINLINE VOID
__DriverSetMemoryKey(
    IN  HANDLE  Key
    )
{
    Driver.MemoryKey = Key;
}

static FORCEINLINE HANDLE
__DriverGetMemoryKey(
    VOID
    )
{
    return Driver.MemoryKey;
}

#define MAXNAMELEN 128

static FORCEINLINE NTSTATUS
__DriverSetPfnArray(
    IN  PCHAR       Name,
    IN  ULONG       Count,
    IN  PFN_NUMBER  PfnArray[]
    )
{
    HANDLE          Key = __DriverGetMemoryKey();
    LONG            Index;
    NTSTATUS        status;

    Index = 0;
    while (Index < (LONG)Count) {
        CHAR    ValueName[MAXNAMELEN];
        PVOID   Value;
        ULONG   Length;

        status = RtlStringCbPrintfA(ValueName,
                                    MAXNAMELEN,
                                    "%s_%u",
                                    Name,
                                    Index);
        ASSERT(NT_SUCCESS(status));

        Value = &PfnArray[Index];
        Length = sizeof (PFN_NUMBER);

        status = RegistryUpdateBinaryValue(Key,
                                           ValueName,
                                           Value,
                                           Length);
        if (!NT_SUCCESS(status))
            goto fail1;

        Info("%s %p\n", ValueName, (PVOID)PfnArray[Index]);

        Index++;
    }

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    while (--Index >= 0) {
        CHAR    ValueName[MAXNAMELEN];

        status = RtlStringCbPrintfA(ValueName,
                                    MAXNAMELEN,
                                    "%s_%u",
                                    Name,
                                    Index);
        ASSERT(NT_SUCCESS(status));

        (VOID) RegistryDeleteValue(Key, ValueName);
    }

    return status;
}

static FORCEINLINE NTSTATUS
__DriverAllocatePfnArray(
    IN  PCHAR           Name,
    IN  ULONG           Count,
    OUT PFN_NUMBER      PfnArray[]
    )
{
    PHYSICAL_ADDRESS    LowAddress;
    PHYSICAL_ADDRESS    HighAddress;
    LARGE_INTEGER       SkipBytes;
    SIZE_T              TotalBytes;
    PMDL                Mdl;
    NTSTATUS            status;

    LowAddress.QuadPart = 0ull;
    HighAddress.QuadPart = ~0ull;
    SkipBytes.QuadPart = 0ull;
    TotalBytes = PAGE_SIZE * Count;

    Mdl = MmAllocatePagesForMdlEx(LowAddress,
                                  HighAddress,
                                  SkipBytes,
                                  TotalBytes,
                                  MmCached,
                                  MM_ALLOCATE_FULLY_REQUIRED);

    status = STATUS_NO_MEMORY;
    if (Mdl == NULL)
        goto fail1;

    if (Mdl->ByteCount < TotalBytes)
        goto fail2;

    ASSERT((Mdl->MdlFlags & (MDL_MAPPED_TO_SYSTEM_VA |
                             MDL_PARTIAL_HAS_BEEN_MAPPED |
                             MDL_PARTIAL |
                             MDL_PARENT_MAPPED_SYSTEM_VA |
                             MDL_SOURCE_IS_NONPAGED_POOL |
                             MDL_IO_SPACE)) == 0);

    status = __DriverSetPfnArray(Name, Count, MmGetMdlPfnArray(Mdl));
    if (!NT_SUCCESS(status))
        goto fail3;

    RtlCopyMemory(PfnArray, MmGetMdlPfnArray(Mdl), sizeof (PFN_NUMBER) * Count);

    ExFreePool(Mdl);

    return STATUS_SUCCESS;

fail3:
    Error("fail3\n");

fail2:
    Error("fail2\n");

    MmFreePagesFromMdl(Mdl);
    ExFreePool(Mdl);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static FORCEINLINE NTSTATUS
__DriverGetPfnArray(
    IN  PCHAR       Name,
    IN  ULONG       Count,
    OUT PFN_NUMBER  PfnArray[]
    )
{
    HANDLE          Key = __DriverGetMemoryKey();
    ULONG           Index;
    NTSTATUS        status;

    for (Index = 0; Index < Count; Index++) {
        CHAR    ValueName[MAXNAMELEN];
        PVOID   Value;
        ULONG   Length;

        status = RtlStringCbPrintfA(ValueName,
                                    MAXNAMELEN,
                                    "%s_%u",
                                    Name,
                                    Index);
        ASSERT(NT_SUCCESS(status));

        status = RegistryQueryBinaryValue(Key,
                                          ValueName,
                                          &Value,
                                          &Length);
        if (!NT_SUCCESS(status))
            goto fail1;

        status = STATUS_UNSUCCESSFUL;
        if (Length != sizeof (PFN_NUMBER))
            goto fail2;

        PfnArray[Index] = *(PPFN_NUMBER)Value;

        RegistryFreeBinaryValue(Value);

        Info("%s %p\n", Name, (PVOID)PfnArray[Index]);
    }

    return STATUS_SUCCESS;

fail2:
    Error("fail2\n");

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

PMDL
DriverGetNamedPages(
    IN  PCHAR   Name,
    IN  ULONG   Count
    )
{
    ULONG       Size;
    PMDL        Mdl;
    PUCHAR      MdlMappedSystemVa;
    NTSTATUS    status;

    Size = sizeof (MDL) + (sizeof (PFN_NUMBER) * Count);
    Mdl = __DriverAllocate(Size);

    status = STATUS_NO_MEMORY;
    if (Mdl == NULL)
        goto fail1;

#pragma warning(push)
#pragma warning(disable:28145) // modifying struct MDL

    Mdl->Size = (USHORT)Size;
    Mdl->MdlFlags = MDL_PAGES_LOCKED;
    Mdl->ByteCount = PAGE_SIZE * Count;

#pragma warning(pop)

    status = __DriverGetPfnArray(Name, Count, MmGetMdlPfnArray(Mdl));
    if (!NT_SUCCESS(status)) {
        if (status == STATUS_OBJECT_NAME_NOT_FOUND)
            status = __DriverAllocatePfnArray(Name, Count,
                                              MmGetMdlPfnArray(Mdl));

        if (!NT_SUCCESS(status))
            goto fail2;
    }

    MdlMappedSystemVa = MmMapLockedPagesSpecifyCache(Mdl,
                                                     KernelMode,
                                                     MmCached,
                                                     NULL,
                                                     FALSE,
                                                     NormalPagePriority);

    status = STATUS_UNSUCCESSFUL;
    if (MdlMappedSystemVa == NULL)
        goto fail3;

    Mdl->StartVa = PAGE_ALIGN(MdlMappedSystemVa);

    return Mdl;

fail3:
    Error("fail3\n");

fail2:
    Error("fail2\n");

    __DriverFree(Mdl);

fail1:
    Error("fail1 (%08x)\n", status);

    return NULL;
}

VOID
DriverPutNamedPages(
    IN  PMDL    Mdl
    )
{
    PUCHAR	    MdlMappedSystemVa;

    ASSERT(Mdl->MdlFlags & MDL_MAPPED_TO_SYSTEM_VA);
    MdlMappedSystemVa = Mdl->MappedSystemVa;

    MmUnmapLockedPages(MdlMappedSystemVa, Mdl);

    // DO NOT FREE PAGES

    __DriverFree(Mdl);
}

XEN_API
NTSTATUS
XenTouch(
    IN  const CHAR  *Name,
    IN  ULONG       MajorVersion,
    IN  ULONG       MinorVersion,
    IN  ULONG       MicroVersion,
    IN  ULONG       BuildNumber
   )
{
    static ULONG    Reference;
    ULONG           Major;
    ULONG           Minor;
    CHAR            Extra[XEN_EXTRAVERSION_LEN];
    NTSTATUS        status;

    status = STATUS_INCOMPATIBLE_DRIVER_BLOCKED;
    if (MajorVersion != MAJOR_VERSION ||
        MinorVersion != MINOR_VERSION ||
        MicroVersion != MICRO_VERSION ||
        BuildNumber != BUILD_NUMBER)
        goto fail1;

    if (Reference != 0)
        goto done;

    status = XenVersion(&Major, &Minor);
    if (status == STATUS_NOT_IMPLEMENTED)
        goto fail2;

    ASSERT(NT_SUCCESS(status));

    status = XenVersionExtra(Extra);
    ASSERT(NT_SUCCESS(status));

    LogPrintf(LOG_LEVEL_INFO,
              "XEN: %u.%u%s (__XEN_INTERFACE_VERSION__ = %08x)\n",
              Major,
              Minor,
              Extra,
              __XEN_INTERFACE_VERSION__);

done:
    Reference++;

    return STATUS_SUCCESS;

fail2:
fail1:
    if (status == STATUS_INCOMPATIBLE_DRIVER_BLOCKED)
        Info("MODULE '%s' NOT COMPATIBLE (REBOOT REQUIRED)\n", Name);

    return status;
}

static VOID
DriverOutputBuffer(
    IN  PVOID   Argument,
    IN  PCHAR   Buffer,
    IN  ULONG   Length
    )
{
    ULONG_PTR   Port = (ULONG_PTR)Argument;

    __outbytestring((USHORT)Port, (PUCHAR)Buffer, Length);
}

#define XEN_PORT    0xE9
#define QEMU_PORT   0x12

NTSTATUS
DllInitialize(
    IN  PUNICODE_STRING RegistryPath
    )
{
    HANDLE              ServiceKey;
    HANDLE              ParametersKey;
    HANDLE              UnplugKey;
    HANDLE              MemoryKey;
    LOG_LEVEL           LogLevel;
    NTSTATUS            status;

    ExInitializeDriverRuntime(DrvRtPoolNxOptIn);
    WdmlibProcgrpInitialize();

    __DbgPrintEnable();

    Trace("====>\n");

    status = LogInitialize();
    if (!NT_SUCCESS(status))
        goto fail1;

    status = RegistryInitialize(RegistryPath);
    if (!NT_SUCCESS(status))
        goto fail2;

    status = RegistryCreateServiceKey(&ServiceKey);
    if (!NT_SUCCESS(status))
        goto fail3;

    status = RegistryCreateSubKey(ServiceKey,
                                  "Parameters",
                                  REG_OPTION_NON_VOLATILE,
                                  &ParametersKey);
    if (!NT_SUCCESS(status))
        goto fail4;

    __DriverSetParametersKey(ParametersKey);

    status = LogReadLogLevel(ParametersKey,
                             "XenLogLevel",
                             &LogLevel);
    if (!NT_SUCCESS(status))
        LogLevel = DEFAULT_XEN_LOG_LEVEL;

    status = LogAddDisposition(LogLevel,
                               DriverOutputBuffer,
                               (PVOID)XEN_PORT,
                               &Driver.XenDisposition);
    ASSERT(NT_SUCCESS(status));

    status = LogReadLogLevel(ParametersKey,
                             "QemuLogLevel",
                             &LogLevel);
    if (!NT_SUCCESS(status))
        LogLevel = DEFAULT_QEMU_LOG_LEVEL;

    status = LogAddDisposition(LogLevel,
                               DriverOutputBuffer,
                               (PVOID)QEMU_PORT,
                               &Driver.QemuDisposition);
    ASSERT(NT_SUCCESS(status));

    Info("%d.%d.%d (%d) (%02d.%02d.%04d)\n",
         MAJOR_VERSION,
         MINOR_VERSION,
         MICRO_VERSION,
         BUILD_NUMBER,
         DAY,
         MONTH,
         YEAR);

    if (__DriverSafeMode())
        Info("SAFE MODE\n");

    status = RegistryCreateSubKey(ServiceKey,
                                  "Unplug",
                                  REG_OPTION_NON_VOLATILE,
                                  &UnplugKey);
    if (!NT_SUCCESS(status))
        goto fail5;

    __DriverSetUnplugKey(UnplugKey);

    status = RegistryCreateSubKey(ServiceKey,
                                  "Memory",
                                  REG_OPTION_VOLATILE,
                                  &MemoryKey);
    if (!NT_SUCCESS(status))
        goto fail6;

    __DriverSetMemoryKey(MemoryKey);

    HypercallInitialize();

    status = AcpiInitialize();
    if (!NT_SUCCESS(status))
        goto fail7;

    status = SystemInitialize();
    if (!NT_SUCCESS(status))
        goto fail8;

    status = BugCheckInitialize();
    if (!NT_SUCCESS(status))
        goto fail9;

    status = ModuleInitialize();
    if (!NT_SUCCESS(status))
        goto fail10;

    status = ProcessInitialize();
    if (!NT_SUCCESS(status))
        goto fail11;

    status = UnplugInitialize();
    if (!NT_SUCCESS(status))
        goto fail12;

    RegistryCloseKey(ServiceKey);

    Trace("<====\n");

    return STATUS_SUCCESS;

fail12:
    Error("fail12\n");

    ProcessTeardown();

fail11:
    Error("fail11\n");

    ModuleTeardown();

fail10:
    Error("fail10\n");

    BugCheckTeardown();

fail9:
    Error("fail9\n");

    SystemTeardown();

fail8:
    Error("fail8\n");

    AcpiTeardown();

fail7:
    Error("fail7\n");

    HypercallTeardown();

    RegistryCloseKey(MemoryKey);
    __DriverSetMemoryKey(NULL);

fail6:
    Error("fail6\n");

    RegistryCloseKey(UnplugKey);
    __DriverSetUnplugKey(NULL);

fail5:
    Error("fail5\n");

    LogRemoveDisposition(Driver.QemuDisposition);
    Driver.QemuDisposition = NULL;

    LogRemoveDisposition(Driver.XenDisposition);
    Driver.XenDisposition = NULL;

    RegistryCloseKey(ParametersKey);
    __DriverSetParametersKey(NULL);

fail4:
    Error("fail4\n");

    RegistryCloseKey(ServiceKey);

fail3:
    Error("fail3\n");

    RegistryTeardown();

fail2:
    Error("fail2\n");

    LogTeardown();

fail1:
    Error("fail1 (%08x)\n", status);

    ASSERT(IsZeroMemory(&Driver, sizeof (XEN_DRIVER)));

    return status;
}

NTSTATUS
DllUnload(
    VOID
    )
{
    HANDLE  MemoryKey;
    HANDLE  UnplugKey;
    HANDLE  ParametersKey;

    Trace("====>\n");

    UnplugTeardown();

    ProcessTeardown();

    ModuleTeardown();

    BugCheckTeardown();

    SystemTeardown();

    AcpiTeardown();

    HypercallTeardown();

    MemoryKey = __DriverGetMemoryKey();

    RegistryCloseKey(MemoryKey);
    __DriverSetMemoryKey(NULL);

    UnplugKey = __DriverGetUnplugKey();

    RegistryCloseKey(UnplugKey);
    __DriverSetUnplugKey(NULL);

    ParametersKey = __DriverGetParametersKey();

    RegistryCloseKey(ParametersKey);
    __DriverSetParametersKey(NULL);

    RegistryTeardown();

    Info("XEN %d.%d.%d (%d) (%02d.%02d.%04d)\n",
         MAJOR_VERSION,
         MINOR_VERSION,
         MICRO_VERSION,
         BUILD_NUMBER,
         DAY,
         MONTH,
         YEAR);

    LogRemoveDisposition(Driver.QemuDisposition);
    Driver.QemuDisposition = NULL;

    LogRemoveDisposition(Driver.XenDisposition);
    Driver.XenDisposition = NULL;

    LogTeardown();

    ASSERT(IsZeroMemory(&Driver, sizeof (XEN_DRIVER)));

    Trace("<====\n");

    return STATUS_SUCCESS;
}

DRIVER_INITIALIZE   DriverEntry;

NTSTATUS
DriverEntry(
    IN  PDRIVER_OBJECT  DriverObject,
    IN  PUNICODE_STRING RegistryPath
    )
{
    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(RegistryPath);

    return STATUS_SUCCESS;
}
