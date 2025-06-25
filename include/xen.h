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

#ifndef _XEN_H
#define _XEN_H

#include <ntddk.h>

#include <xen-version.h>
#include <xen-types.h>
#include <xen-warnings.h>
#include <xen-errno.h>

#include <public/errno.h>
#include <public/xen.h>
#include <public/memory.h>
#include <public/event_channel.h>
#include <public/grant_table.h>
#include <public/sched.h>
#include <public/vcpu.h>
#include <public/hvm/params.h>
#include <public/hvm/hvm_info_table.h>

// xs_wire.h gates the definition of the xsd_errors enumeration
// on whether EINVAL is defined. Unfortunately EINVAL is actually
// part of an enumeration and the #ifdef test thus fails.
// Override the enumeration value here with a #define.

#define EINVAL  XEN_EINVAL

#include <public/io/xs_wire.h>
#include <public/io/console.h>
#include <public/version.h>

#ifndef XEN_API
#define XEN_API __declspec(dllimport)
#endif  // XEN_API

// Dummy function to cause XEN.SYS to be loaded and initialized
XEN_API
NTSTATUS
XenTouch(
    _In_ PCSTR      Name,
    _In_ ULONG      MajorVersion,
    _In_ ULONG      MinorVersion,
    _In_ ULONG      MicroVersion,
    _In_ ULONG      BuildNumber
    );

// HYPERCALL

XEN_API
VOID
HypercallPopulate(
    VOID
    );

// HVM

_Check_return_
XEN_API
NTSTATUS
HvmSetParam(
    _In_ ULONG      Parameter,
    _In_ ULONGLONG  Value
    );

_Check_return_
XEN_API
NTSTATUS
HvmGetParam(
    _In_ ULONG          Parameter,
    _Out_ PULONGLONG    Value
    );

_Check_return_
XEN_API
NTSTATUS
HvmPagetableDying(
    _In_ PHYSICAL_ADDRESS   Address
    );

_Check_return_
XEN_API
NTSTATUS
HvmSetEvtchnUpcallVector(
    _In_ unsigned int   vcpu_id,
    _In_ UCHAR          Vector
    );

// MEMORY

_Check_return_
XEN_API
NTSTATUS
MemoryAddToPhysmap(
    _In_ PFN_NUMBER Pfn,
    _In_ ULONG      Space,
    _In_ ULONG_PTR  Offset
    );

_Check_return_
XEN_API
NTSTATUS
MemoryRemoveFromPhysmap(
    _In_ PFN_NUMBER Pfn
    );

#define PAGE_ORDER_4K   0
#define PAGE_ORDER_2M   9

_Check_return_
XEN_API
ULONG
MemoryDecreaseReservation(
    _In_ ULONG          Order,
    _In_ ULONG          Count,
    _In_ PPFN_NUMBER    PfnArray
    );

_Check_return_
XEN_API
ULONG
MemoryPopulatePhysmap(
    _In_ ULONG          Order,
    _In_ ULONG          Count,
    _In_ PPFN_NUMBER    PfnArray
    );

// EVENT CHANNEL

_Check_return_
XEN_API
NTSTATUS
EventChannelSend(
    _In_ ULONG  Port
    );

_Check_return_
XEN_API
NTSTATUS
EventChannelAllocateUnbound(
    _In_ USHORT Domain,
    _Out_ ULONG *Port
    );

_Check_return_
XEN_API
NTSTATUS
EventChannelBindInterDomain(
    _In_ USHORT RemoteDomain,
    _In_ ULONG  RemotePort,
    _Out_ ULONG *LocalPort
    );

_Check_return_
XEN_API
NTSTATUS
EventChannelBindVirq(
    _In_ ULONG          Virq,
    _In_ unsigned int   vcpu_id,
    _Out_ ULONG         *LocalPort
    );

_Check_return_
XEN_API
NTSTATUS
EventChannelQueryInterDomain(
    _In_ ULONG      LocalPort,
    _Out_ USHORT    *RemoteDomain,
    _Out_ ULONG     *RemotePort
    );

_Check_return_
XEN_API
NTSTATUS
EventChannelClose(
    _In_ ULONG  LocalPort
    );

_Check_return_
XEN_API
NTSTATUS
EventChannelExpandArray(
    _In_ PFN_NUMBER Pfn
    );

_Check_return_
XEN_API
NTSTATUS
EventChannelInitControl(
    _In_ PFN_NUMBER     Pfn,
    _In_ unsigned int   vcpu_id
    );

_Check_return_
XEN_API
NTSTATUS
EventChannelReset(
    VOID
    );

_Check_return_
XEN_API
NTSTATUS
EventChannelBindVirtualCpu(
    _In_ ULONG          LocalPort,
    _In_ unsigned int   vcpu_id
    );

_Check_return_
XEN_API
NTSTATUS
EventChannelUnmask(
    _In_ ULONG  LocalPort
    );

// GRANT TABLE

_Check_return_
XEN_API
NTSTATUS
GrantTableSetVersion(
    _In_ uint32_t   Version
    );

_Check_return_
XEN_API
NTSTATUS
GrantTableGetVersion(
    _Out_ uint32_t  *Version
    );

_Check_return_
XEN_API
NTSTATUS
GrantTableCopy(
    _In_ struct gnttab_copy op[],
    _In_ ULONG              Count
    );

_Check_return_
XEN_API
NTSTATUS
GrantTableMapForeignPage(
    _In_ USHORT             Domain,
    _In_ ULONG              GrantRef,
    _In_ PHYSICAL_ADDRESS   Address,
    _In_ BOOLEAN            ReadOnly,
    _Out_ ULONG             *Handle
    );

_Check_return_
XEN_API
NTSTATUS
GrantTableUnmapForeignPage(
    _In_ ULONG              Handle,
    _In_ PHYSICAL_ADDRESS   Address
    );

_Check_return_
XEN_API
NTSTATUS
GrantTableQuerySize(
    _Out_opt_ uint32_t      *Current,
    _Out_opt_ uint32_t      *Maximum
    );

// SCHED

_Check_return_
XEN_API
NTSTATUS
SchedShutdownCode(
    _In_ ULONG  Reason
    );

_Check_return_
XEN_API
NTSTATUS
SchedShutdown(
    _In_ ULONG  Reason
    );

XEN_API
VOID
SchedYield(
    VOID
    );

XEN_API
NTSTATUS
SchedWatchdog(
    _Inout_ PULONG  Id,
    _In_ ULONG      Seconds
    );

// XEN VERSION

_Check_return_
XEN_API
NTSTATUS
XenVersion(
    _Out_ PULONG    Major,
    _Out_ PULONG    Minor
    );

_Check_return_
XEN_API
NTSTATUS
XenVersionExtra(
    _Out_writes_z_(XEN_EXTRAVERSION_LEN) PSTR   Extra
    );

// MODULE

XEN_API
VOID
ModuleLookup(
    _In_ ULONG_PTR          Address,
    _Outptr_result_z_ PSTR  *Name,
    _Out_ PULONG_PTR        Offset
    );

// UNPLUG

typedef enum _UNPLUG_TYPE {
    UNPLUG_DISKS = 0,
    UNPLUG_NICS,
    UNPLUG_TYPE_COUNT
} UNPLUG_TYPE, *PUNPLUG_TYPE;

XEN_API
VOID
UnplugDevices(
    VOID
    );

XEN_API
NTSTATUS
UnplugIncrementValue(
    _In_ UNPLUG_TYPE    Type
    );

XEN_API
NTSTATUS
UnplugDecrementValue(
    _In_ UNPLUG_TYPE    Type
    );

XEN_API
BOOLEAN
UnplugGetRequest(
    _In_ UNPLUG_TYPE    Type
    );

// LOG

typedef enum _LOG_LEVEL {
    LOG_LEVEL_NONE = 0,
    LOG_LEVEL_TRACE = 1 << DPFLTR_TRACE_LEVEL,
    LOG_LEVEL_INFO = 1 << DPFLTR_INFO_LEVEL,
    LOG_LEVEL_WARNING = 1 << DPFLTR_WARNING_LEVEL,
    LOG_LEVEL_ERROR = 1 << DPFLTR_ERROR_LEVEL,
    LOG_LEVEL_CRITICAL = 0x80000000
} LOG_LEVEL, *PLOG_LEVEL;

XEN_API
VOID
LogCchVPrintf(
    _In_ LOG_LEVEL  Level,
    _In_ ULONG      Count,
    _In_ PCSTR      Format,
    _In_ va_list    Arguments
    );

XEN_API
VOID
LogVPrintf(
    _In_ LOG_LEVEL  Level,
    _In_ PCSTR      Format,
    _In_ va_list    Arguments
    );

XEN_API
VOID
LogCchPrintf(
    _In_ LOG_LEVEL  Level,
    _In_ ULONG      Count,
    _In_ PCSTR      Format,
    ...
    );

XEN_API
VOID
LogPrintf(
    _In_ LOG_LEVEL  Level,
    _In_ PCSTR      Format,
    ...
    );

XEN_API
VOID
LogResume(
    VOID
    );

XEN_API
NTSTATUS
LogReadLogLevel(
    _In_ HANDLE         Key,
    _In_ PSTR           Name,
    _Out_ PLOG_LEVEL    LogLevel
    );

typedef struct _LOG_DISPOSITION LOG_DISPOSITION, *PLOG_DISPOSITION;

XEN_API
NTSTATUS
LogAddDisposition(
    _In_ LOG_LEVEL              Mask,
    _In_ VOID                   (*Function)(PVOID, PSTR, ULONG),
    _In_opt_ PVOID              Argument,
    _Outptr_ PLOG_DISPOSITION   *Disposition
    );

XEN_API
VOID
LogRemoveDisposition(
    _In_ PLOG_DISPOSITION   Disposition
    );


// SYSTEM

XEN_API
NTSTATUS
SystemProcessorVcpuId(
    _In_ ULONG          Cpu,
    _Out_ unsigned int  *vcpu_id
    );

XEN_API
NTSTATUS
SystemProcessorVcpuInfo(
    _In_ ULONG          Cpu,
    _Out_ vcpu_info_t   **Vcpu
    );

XEN_API
NTSTATUS
SystemProcessorRegisterVcpuInfo(
    _In_ ULONG      Cpu,
    _In_ BOOLEAN    Force
    );

XEN_API
PHYSICAL_ADDRESS
SystemMaximumPhysicalAddress(
    VOID
    );

XEN_API
BOOLEAN
SystemRealTimeIsUniversal(
    VOID
    );

XEN_API
NTSTATUS
SystemSetWatchdog(
    _In_ ULONG      Seconds
    );

XEN_API
VOID
SystemStopWatchdog(
    VOID
    );

// VCPU

_Check_return_
XEN_API
NTSTATUS
VcpuSetPeriodicTimer(
    _In_ unsigned int       vcpu_id,
    _In_opt_ PLARGE_INTEGER Period
    );

_Check_return_
XEN_API
NTSTATUS
VcpuRegisterVcpuInfo(
    _In_ unsigned int               vcpu_id,
    _In_ PFN_NUMBER                 Pfn,
    _In_ ULONG                      Offset
    );

// FILTERS

XEN_API
VOID
FiltersInstall(
     VOID
     );

XEN_API
VOID
FiltersUninstall(
     VOID
     );

// CONFIG

XEN_API
NTSTATUS
ConfigGetActive(
    _In_ PCSTR              Key,
    _Outptr_result_z_ PSTR  *Value
    );

XEN_API
NTSTATUS
ConfigSetActive(
    _In_ PSTR   DeviceID,
    _In_ PSTR   InstanceID,
    _In_ PSTR   LocationInformation
    );

XEN_API
NTSTATUS
ConfigUpdateActive(
    _In_ PSTR   DeviceID,
    _In_ PSTR   InstanceID,
    _In_ PSTR   LocationInformation
    );

XEN_API
NTSTATUS
ConfigClearActive(
    VOID
    );

XEN_API
NTSTATUS
ConfigRequestReboot(
    _In_ HANDLE     ParametersKey,
    _In_ PSTR       Module
    );

XEN_API
NTSTATUS
ConfigQuerySystemStartOption(
    _In_ PSTR               Key,
    _Outptr_ PANSI_STRING   *Option
    );

#endif  // _XEN_H
