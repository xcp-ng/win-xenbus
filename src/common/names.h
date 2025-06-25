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

#ifndef _COMMON_NAMES_H_
#define _COMMON_NAMES_H_

#include <ntddk.h>
#include <xen.h>

static FORCEINLINE const CHAR *
PowerStateTypeName(
    _In_ POWER_STATE_TYPE   Type
    )
{
#define _POWER_TYPE_NAME(_Type) \
        case _Type:             \
            return #_Type;

    switch (Type) {
    _POWER_TYPE_NAME(SystemPowerState);
    _POWER_TYPE_NAME(DevicePowerState);
    default:
        break;
    }

    return ("UNKNOWN");
#undef  _POWER_ACTION_NAME
}

static FORCEINLINE const CHAR *
SystemPowerStateName(
    _In_ SYSTEM_POWER_STATE State
    )
{
#define _POWER_SYSTEM_STATE_NAME(_State)    \
        case PowerSystem ## _State:         \
            return #_State;

    switch (State) {
    _POWER_SYSTEM_STATE_NAME(Unspecified);
    _POWER_SYSTEM_STATE_NAME(Working);
    _POWER_SYSTEM_STATE_NAME(Sleeping1);
    _POWER_SYSTEM_STATE_NAME(Sleeping2);
    _POWER_SYSTEM_STATE_NAME(Sleeping3);
    _POWER_SYSTEM_STATE_NAME(Hibernate);
    _POWER_SYSTEM_STATE_NAME(Shutdown);
    _POWER_SYSTEM_STATE_NAME(Maximum);
    default:
        break;
    }

    return ("UNKNOWN");
#undef  _POWER_SYSTEM_STATE_NAME
}

static FORCEINLINE const CHAR *
DevicePowerStateName(
    _In_ DEVICE_POWER_STATE State
    )
{
#define _POWER_DEVICE_STATE_NAME(_State)    \
        case PowerDevice ## _State:         \
            return #_State;

    switch (State) {
    _POWER_DEVICE_STATE_NAME(Unspecified);
    _POWER_DEVICE_STATE_NAME(D0);
    _POWER_DEVICE_STATE_NAME(D1);
    _POWER_DEVICE_STATE_NAME(D2);
    _POWER_DEVICE_STATE_NAME(D3);
    _POWER_DEVICE_STATE_NAME(Maximum);
    default:
        break;
    }

    return ("UNKNOWN");
#undef  _POWER_DEVICE_STATE_NAME
}

static FORCEINLINE const CHAR *
PowerActionName(
    _In_ POWER_ACTION   Type
    )
{
#define _POWER_ACTION_NAME(_Type)   \
        case PowerAction ## _Type:  \
            return #_Type;

    switch (Type) {
    _POWER_ACTION_NAME(None);
    _POWER_ACTION_NAME(Reserved);
    _POWER_ACTION_NAME(Sleep);
    _POWER_ACTION_NAME(Hibernate);
    _POWER_ACTION_NAME(Shutdown);
    _POWER_ACTION_NAME(ShutdownReset);
    _POWER_ACTION_NAME(ShutdownOff);
    _POWER_ACTION_NAME(WarmEject);
    default:
        break;
    }

    return ("UNKNOWN");
#undef  _POWER_ACTION_NAME
}

static FORCEINLINE const CHAR *
PowerMinorFunctionName(
    _In_ ULONG  MinorFunction
    )
{
#define _POWER_MINOR_FUNCTION_NAME(_Function)   \
    case IRP_MN_ ## _Function:                  \
        return #_Function;

    switch (MinorFunction) {
    _POWER_MINOR_FUNCTION_NAME(WAIT_WAKE);
    _POWER_MINOR_FUNCTION_NAME(POWER_SEQUENCE);
    _POWER_MINOR_FUNCTION_NAME(SET_POWER);
    _POWER_MINOR_FUNCTION_NAME(QUERY_POWER);

    default:
        return "UNKNOWN";
    }

#undef  _POWER_MINOR_FUNCTION_NAME
}

static FORCEINLINE const CHAR *
PnpMinorFunctionName(
    _In_ ULONG  Function
    )
{
#define _PNP_MINOR_FUNCTION_NAME(_Function) \
    case IRP_MN_ ## _Function:              \
        return #_Function;

    switch (Function) {
    _PNP_MINOR_FUNCTION_NAME(START_DEVICE);
    _PNP_MINOR_FUNCTION_NAME(QUERY_REMOVE_DEVICE);
    _PNP_MINOR_FUNCTION_NAME(REMOVE_DEVICE);
    _PNP_MINOR_FUNCTION_NAME(CANCEL_REMOVE_DEVICE);
    _PNP_MINOR_FUNCTION_NAME(STOP_DEVICE);
    _PNP_MINOR_FUNCTION_NAME(QUERY_STOP_DEVICE);
    _PNP_MINOR_FUNCTION_NAME(CANCEL_STOP_DEVICE);
    _PNP_MINOR_FUNCTION_NAME(QUERY_DEVICE_RELATIONS);
    _PNP_MINOR_FUNCTION_NAME(QUERY_INTERFACE);
    _PNP_MINOR_FUNCTION_NAME(QUERY_CAPABILITIES);
    _PNP_MINOR_FUNCTION_NAME(QUERY_RESOURCES);
    _PNP_MINOR_FUNCTION_NAME(QUERY_RESOURCE_REQUIREMENTS);
    _PNP_MINOR_FUNCTION_NAME(QUERY_DEVICE_TEXT);
    _PNP_MINOR_FUNCTION_NAME(FILTER_RESOURCE_REQUIREMENTS);
    _PNP_MINOR_FUNCTION_NAME(READ_CONFIG);
    _PNP_MINOR_FUNCTION_NAME(WRITE_CONFIG);
    _PNP_MINOR_FUNCTION_NAME(EJECT);
    _PNP_MINOR_FUNCTION_NAME(SET_LOCK);
    _PNP_MINOR_FUNCTION_NAME(QUERY_ID);
    _PNP_MINOR_FUNCTION_NAME(QUERY_PNP_DEVICE_STATE);
    _PNP_MINOR_FUNCTION_NAME(QUERY_BUS_INFORMATION);
    _PNP_MINOR_FUNCTION_NAME(DEVICE_USAGE_NOTIFICATION);
    _PNP_MINOR_FUNCTION_NAME(SURPRISE_REMOVAL);
    _PNP_MINOR_FUNCTION_NAME(QUERY_LEGACY_BUS_INFORMATION);
    default:
        break;
    }

    return "UNKNOWN";

#undef  _PNP_MINOR_FUNCTION_NAME
}

static FORCEINLINE const CHAR *
ResourceDescriptorTypeName(
    _In_ UCHAR  Type
    )
{
#define _RESOURCE_DESCRIPTOR_TYPE_NAME(_Type)   \
    case CmResourceType ## _Type:               \
        return #_Type;

    switch (Type) {
    _RESOURCE_DESCRIPTOR_TYPE_NAME(Null);
    _RESOURCE_DESCRIPTOR_TYPE_NAME(Port);
    _RESOURCE_DESCRIPTOR_TYPE_NAME(Interrupt);
    _RESOURCE_DESCRIPTOR_TYPE_NAME(Memory);
    _RESOURCE_DESCRIPTOR_TYPE_NAME(Dma);
    _RESOURCE_DESCRIPTOR_TYPE_NAME(DeviceSpecific);
    _RESOURCE_DESCRIPTOR_TYPE_NAME(BusNumber);
    _RESOURCE_DESCRIPTOR_TYPE_NAME(MemoryLarge);
    _RESOURCE_DESCRIPTOR_TYPE_NAME(ConfigData);
    _RESOURCE_DESCRIPTOR_TYPE_NAME(DevicePrivate);
    default:
        break;
    }

    return "UNKNOWN";

#undef  _RESOURCE_DESCRIPTOR_TYPE_NAME
}

static FORCEINLINE const CHAR *
ResourceDescriptorShareDispositionName(
    _In_ UCHAR  Disposition
    )
{
#define _RESOURCE_DESCRIPTOR_SHARE_DISPOSITION_NAME(_Disposition)  \
    case CmResourceShare ## _Disposition:                           \
        return #_Disposition;

    switch (Disposition) {
    _RESOURCE_DESCRIPTOR_SHARE_DISPOSITION_NAME(Undetermined);
    _RESOURCE_DESCRIPTOR_SHARE_DISPOSITION_NAME(DeviceExclusive);
    _RESOURCE_DESCRIPTOR_SHARE_DISPOSITION_NAME(DriverExclusive);
    _RESOURCE_DESCRIPTOR_SHARE_DISPOSITION_NAME(Shared);
    default:
        break;
    }

    return "UNKNOWN";

#undef  _RESOURCE_DESCRIPTOR_SHARE_DISPOSITION_NAME
}

static FORCEINLINE const CHAR *
IrqDevicePolicyName(
    _In_ IRQ_DEVICE_POLICY  Policy
    )
{
#define _IRQ_DEVICE_POLICY_NAME(_Policy)    \
    case IrqPolicy ## _Policy:              \
        return #_Policy;

    switch (Policy) {
    _IRQ_DEVICE_POLICY_NAME(MachineDefault);
    _IRQ_DEVICE_POLICY_NAME(AllCloseProcessors);
    _IRQ_DEVICE_POLICY_NAME(OneCloseProcessor);
    _IRQ_DEVICE_POLICY_NAME(AllProcessorsInMachine);
    _IRQ_DEVICE_POLICY_NAME(SpecifiedProcessors);
    _IRQ_DEVICE_POLICY_NAME(SpreadMessagesAcrossAllProcessors);
    default:
        break;
    }

    return "UNKNOWN";

#undef  _IRQ_DEVICE_POLICY_NAME
}

static FORCEINLINE const CHAR *
IrqPriorityName(
    _In_ IRQ_PRIORITY   Priority
    )
{
#define _IRQ_PRIORITY_NAME(_Priority)   \
    case IrqPriority ## _Priority:      \
        return #_Priority;

    switch (Priority) {
    _IRQ_PRIORITY_NAME(Undefined);
    _IRQ_PRIORITY_NAME(Low);
    _IRQ_PRIORITY_NAME(Normal);
    _IRQ_PRIORITY_NAME(High);
    default:
        break;
    }

    return "UNKNOWN";

#undef  _IRQ_PRIORITY_NAME
}

static FORCEINLINE const CHAR *
InterruptModeName(
    _In_ KINTERRUPT_MODE    Mode
    )
{
#define _INTERRUPT_MODE_NAME(_Mode) \
    case _Mode:                     \
        return #_Mode;

    switch (Mode) {
    _INTERRUPT_MODE_NAME(LevelSensitive);
    _INTERRUPT_MODE_NAME(Latched);
    default:
        break;
    }

    return "UNKNOWN";

#undef  _INTERRUPT_MODE_NAME
}

static FORCEINLINE const CHAR *
DeviceUsageNotificationTypeName(
    _In_ DEVICE_USAGE_NOTIFICATION_TYPE Type
    )
{
#define _DEVICE_USAGE_TYPE_NAME(_Type)  \
    case DeviceUsageType ## _Type:      \
        return #_Type;

    switch (Type) {
    _DEVICE_USAGE_TYPE_NAME(Paging);
    _DEVICE_USAGE_TYPE_NAME(Hibernation);
    _DEVICE_USAGE_TYPE_NAME(DumpFile);
    default:
        break;
    }

    return "UNKNOWN";

#undef  _DEVICE_USAGE_TYPE_NAME
}

static FORCEINLINE const CHAR *
InterfaceTypeName(
    _In_ INTERFACE_TYPE Type
    )
{
#define _INTERFACE_TYPE_NAME(_Type) \
    case _Type:                     \
        return #_Type;

    switch (Type) {
    _INTERFACE_TYPE_NAME(InterfaceTypeUndefined);
    _INTERFACE_TYPE_NAME(Internal);
    _INTERFACE_TYPE_NAME(Isa);
    _INTERFACE_TYPE_NAME(Eisa);
    _INTERFACE_TYPE_NAME(MicroChannel);
    _INTERFACE_TYPE_NAME(TurboChannel);
    _INTERFACE_TYPE_NAME(PCIBus);
    _INTERFACE_TYPE_NAME(VMEBus);
    _INTERFACE_TYPE_NAME(NuBus);
    _INTERFACE_TYPE_NAME(PCMCIABus);
    _INTERFACE_TYPE_NAME(CBus);
    _INTERFACE_TYPE_NAME(MPIBus);
    _INTERFACE_TYPE_NAME(MPSABus);
    _INTERFACE_TYPE_NAME(ProcessorInternal);
    _INTERFACE_TYPE_NAME(InternalPowerBus);
    _INTERFACE_TYPE_NAME(PNPISABus);
    _INTERFACE_TYPE_NAME(PNPBus);
    _INTERFACE_TYPE_NAME(Vmcs);
    _INTERFACE_TYPE_NAME(ACPIBus);
    default:
        break;
    }

    return "UNKNOWN";

#undef  _INTERFACE_TYPE_NAME
}

static FORCEINLINE const CHAR *
DmaWidthName(
    _In_ DMA_WIDTH  Width
    )
{
#define _DMA_WIDTH_NAME(_Width) \
    case Width ## _Width:       \
        return #_Width;

    switch (Width) {
    _DMA_WIDTH_NAME(8Bits);
    _DMA_WIDTH_NAME(16Bits);
    _DMA_WIDTH_NAME(32Bits);
    _DMA_WIDTH_NAME(64Bits);
    _DMA_WIDTH_NAME(NoWrap);
    default:
        break;
    }

    return "UNKNOWN";

#undef  _DMA_WIDTH_NAME
}

static FORCEINLINE const CHAR *
DmaSpeedName(
    _In_ DMA_SPEED  Speed
    )
{
#define _DMA_SPEED_NAME(_Speed) \
    case _Speed:                \
        return #_Speed;

    switch (Speed) {
    _DMA_SPEED_NAME(Compatible);
    _DMA_SPEED_NAME(TypeA);
    _DMA_SPEED_NAME(TypeB);
    _DMA_SPEED_NAME(TypeC);
    _DMA_SPEED_NAME(TypeF);
    _DMA_SPEED_NAME(MaximumDmaSpeed);
    default:
        break;
    }

    return "UNKNOWN";

#undef  _DMA_SPEED_NAME
}

static FORCEINLINE const CHAR *
BusQueryIdTypeName(
    _In_ BUS_QUERY_ID_TYPE  Type
    )
{
#define _BUS_QUERY_ID_TYPE_NAME(_Type)  \
    case BusQuery ## _Type:             \
        return #_Type;

    switch (Type) {
    _BUS_QUERY_ID_TYPE_NAME(DeviceID);
    _BUS_QUERY_ID_TYPE_NAME(HardwareIDs);
    _BUS_QUERY_ID_TYPE_NAME(CompatibleIDs);
    _BUS_QUERY_ID_TYPE_NAME(InstanceID);
    _BUS_QUERY_ID_TYPE_NAME(DeviceSerialNumber);
    _BUS_QUERY_ID_TYPE_NAME(ContainerID);
    default:
        break;
    }

    return "UNKNOWN";

#undef  _BUS_QUERY_ID_TYPE_NAME
}

static FORCEINLINE const CHAR *
ProcessorChangeName(
    _In_ KE_PROCESSOR_CHANGE_NOTIFY_STATE   Change
    )
{
#define _PROCESSOR_CHANGE_NAME(_Change) \
    case KeProcessor ## _Change:        \
        return #_Change;

    switch (Change) {
    _PROCESSOR_CHANGE_NAME(AddStartNotify);
    _PROCESSOR_CHANGE_NAME(AddCompleteNotify);
    _PROCESSOR_CHANGE_NAME(AddFailureNotify);
    default:
        break;
    }

    return "UNKNOWN";

#undef _PROCESSOR_CHANGE_NAME
}

static FORCEINLINE const CHAR *
VirqName(
    _In_ ULONG  Type
    )
{
#define _VIRQ_NAME(_Type) \
    case VIRQ_ ## _Type:  \
        return #_Type;

    switch (Type) {
    _VIRQ_NAME(DEBUG);
    _VIRQ_NAME(TIMER);
    default:
        break;
    }

    return "UNKNOWN";

#undef _VIRQ_NAME
}

#endif // _COMMON_NAMES_H_
