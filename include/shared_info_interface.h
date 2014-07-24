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

/*! \file shared_info_interface.h
    \brief XENBUS SHARED_INFO Interface

    This interface provides access to the hypervisor shared info
*/

#ifndef _XENBUS_SHARED_INFO_INTERFACE_H
#define _XENBUS_SHARED_INFO_INTERFACE_H

#ifndef _WINDLL

#define XENBUS_SHARED_INFO_EVTCHN_PER_SELECTOR     (sizeof (ULONG_PTR) * 8)
#define XENBUS_SHARED_INFO_EVTCHN_SELECTOR_COUNT   (RTL_FIELD_SIZE(shared_info_t, evtchn_pending) / sizeof (ULONG_PTR))

/*! \typedef XENBUS_SHARED_INFO_ACQUIRE
    \brief Acquire a reference to the SHARED_INFO interface

    \param Interface The interface header
*/  
typedef NTSTATUS
(*XENBUS_SHARED_INFO_ACQUIRE)(
    IN  PINTERFACE  Interface
    );

/*! \typedef XENBUS_SHARED_INFO_RELEASE
    \brief Release a reference to the SHARED_INFO interface

    \param Interface The interface header
*/  
typedef VOID
(*XENBUS_SHARED_INFO_RELEASE)(
    IN  PINTERFACE  Interface
    );

/*! \typedef XENBUS_SHARED_INFO_EVTCHN_POLL
    \brief Private method for EVTCHN inerface
*/  
typedef BOOLEAN
(*XENBUS_SHARED_INFO_EVTCHN_POLL)(
    IN  PINTERFACE  Interface,
    IN  BOOLEAN     (*Function)(PVOID, ULONG),
    IN  PVOID       Argument
    );

/*! \typedef XENBUS_SHARED_INFO_EVTCHN_ACK
    \brief Private method for EVTCHN inerface
*/  
typedef VOID
(*XENBUS_SHARED_INFO_EVTCHN_ACK)(
    IN  PINTERFACE  Interface,
    IN  ULONG       Port
    );

/*! \typedef XENBUS_SHARED_INFO_EVTCHN_MASK
    \brief Private method for EVTCHN inerface
*/  
typedef VOID
(*XENBUS_SHARED_INFO_EVTCHN_MASK)(
    IN  PINTERFACE  Interface,
    IN  ULONG       Port
    );

/*! \typedef XENBUS_SHARED_INFO_EVTCHN_UNMASK
    \brief Private method for EVTCHN inerface
*/  
typedef BOOLEAN
(*XENBUS_SHARED_INFO_EVTCHN_UNMASK)(
    IN  PINTERFACE  Interface,
    IN  ULONG       Port
    );

/*! \typedef XENBUS_SHARED_INFO_GET_TIME
    \brief Return the wallclock time from the shared info

    \param Interface The interface header
    \return The wallclock time in units of 100ns
*/  
typedef LARGE_INTEGER
(*XENBUS_SHARED_INFO_GET_TIME)(
    IN  PINTERFACE  Interface
    );

// {7E73C34F-1640-4649-A8F3-263BC930A004}
DEFINE_GUID(GUID_XENBUS_SHARED_INFO_INTERFACE, 
0x7e73c34f, 0x1640, 0x4649, 0xa8, 0xf3, 0x26, 0x3b, 0xc9, 0x30, 0xa0, 0x4);

/*! \struct _XENBUS_SHARED_INFO_INTERFACE_V1
    \brief SHARED_INFO interface version 1
*/
struct _XENBUS_SHARED_INFO_INTERFACE_V1 {
    INTERFACE                           Interface;
    XENBUS_SHARED_INFO_ACQUIRE          SharedInfoAcquire;
    XENBUS_SHARED_INFO_RELEASE          SharedInfoRelease;
    XENBUS_SHARED_INFO_EVTCHN_POLL      SharedInfoEvtchnPoll;
    XENBUS_SHARED_INFO_EVTCHN_ACK       SharedInfoEvtchnAck;
    XENBUS_SHARED_INFO_EVTCHN_MASK      SharedInfoEvtchnMask;
    XENBUS_SHARED_INFO_EVTCHN_UNMASK    SharedInfoEvtchnUnmask;
    XENBUS_SHARED_INFO_GET_TIME         SharedInfoGetTime;
};

typedef struct _XENBUS_SHARED_INFO_INTERFACE_V1 XENBUS_SHARED_INFO_INTERFACE, *PXENBUS_SHARED_INFO_INTERFACE;

/*! \def XENBUS_SHARED_INFO
    \brief Macro at assist in method invocation
*/
#define XENBUS_SHARED_INFO(_Method, _Interface, ...)    \
    (_Interface)->SharedInfo ## _Method((PINTERFACE)(_Interface), __VA_ARGS__)

#endif  // _WINDLL

#define XENBUS_SHARED_INFO_INTERFACE_VERSION_MIN    1
#define XENBUS_SHARED_INFO_INTERFACE_VERSION_MAX    1

#endif  // _XENBUS_SHARED_INFO_H
