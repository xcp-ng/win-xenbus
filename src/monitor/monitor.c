/* Copyright (c) Citrix Systems Inc.
 * All rights reserved.
 *
 * Redistribution and use in source 1and binary forms,
 * with or without modification, are permitted provided
 * that the following conditions are met:
 *
 * *   Redistributions of source code must retain the above
 *     copyright notice, this list of conditions and the23
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

#include <windows.h>
#include <tchar.h>
#include <stdlib.h>
#include <strsafe.h>
#include <wtsapi32.h>
#include <malloc.h>
#include <assert.h>

#include <version.h>

#include "messages.h"
#include "strings.h"

#define MONITOR_NAME        __MODULE__
#define MONITOR_DISPLAYNAME MONITOR_NAME

typedef struct _MONITOR_CONTEXT {
    SERVICE_STATUS          Status;
    SERVICE_STATUS_HANDLE   Service;
    HANDLE                  EventLog;
    HANDLE                  StopEvent;
    HANDLE                  RequestEvent;
    HKEY                    RequestKey;
    BOOL                    RebootPending;
} MONITOR_CONTEXT, *PMONITOR_CONTEXT;

MONITOR_CONTEXT MonitorContext;

#define MAXIMUM_BUFFER_SIZE 1024

#define SERVICES_KEY "SYSTEM\\CurrentControlSet\\Services"

#define SERVICE_KEY(_Service) \
        SERVICES_KEY ## "\\" ## #_Service

#define REQUEST_KEY \
        SERVICE_KEY(XENBUS_MONITOR) ## "\\Request"

static VOID
#pragma prefast(suppress:6262) // Function uses '1036' bytes of stack: exceeds /analyze:stacksize'1024'
__Log(
    IN  const CHAR      *Format,
    IN  ...
    )
{
#if DBG
    PMONITOR_CONTEXT    Context = &MonitorContext;
    const TCHAR         *Strings[1];
#endif
    TCHAR               Buffer[MAXIMUM_BUFFER_SIZE];
    va_list             Arguments;
    size_t              Length;
    HRESULT             Result;

    va_start(Arguments, Format);
    Result = StringCchVPrintf(Buffer, MAXIMUM_BUFFER_SIZE, Format, Arguments);
    va_end(Arguments);

    if (Result != S_OK && Result != STRSAFE_E_INSUFFICIENT_BUFFER)
        return;

    Result = StringCchLength(Buffer, MAXIMUM_BUFFER_SIZE, &Length);
    if (Result != S_OK)
        return;

    Length = __min(MAXIMUM_BUFFER_SIZE - 1, Length + 2);

    __analysis_assume(Length < MAXIMUM_BUFFER_SIZE);
    __analysis_assume(Length >= 2);
    Buffer[Length] = '\0';
    Buffer[Length - 1] = '\n';
    Buffer[Length - 2] = '\r';

    OutputDebugString(Buffer);

#if DBG
    Strings[0] = Buffer;

    if (Context->EventLog != NULL)
        ReportEvent(Context->EventLog,
                    EVENTLOG_INFORMATION_TYPE,
                    0,
                    MONITOR_LOG,
                    NULL,
                    ARRAYSIZE(Strings),
                    0,
                    Strings,
                    NULL);
#endif
}

#define Log(_Format, ...) \
        __Log(__MODULE__ "|" __FUNCTION__ ": " _Format, __VA_ARGS__)

static PTCHAR
GetErrorMessage(
    IN  HRESULT Error
    )
{
    PTCHAR      Message;
    ULONG       Index;

    FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                  FORMAT_MESSAGE_FROM_SYSTEM |
                  FORMAT_MESSAGE_IGNORE_INSERTS,
                  NULL,
                  Error,
                  MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                  (LPTSTR)&Message,
                  0,
                  NULL);

    for (Index = 0; Message[Index] != '\0'; Index++) {
        if (Message[Index] == '\r' || Message[Index] == '\n') {
            Message[Index] = '\0';
            break;
        }
    }

    return Message;
}

static const CHAR *
ServiceStateName(
    IN  DWORD   State
    )
{
#define _STATE_NAME(_State) \
    case SERVICE_ ## _State: \
        return #_State

    switch (State) {
    _STATE_NAME(START_PENDING);
    _STATE_NAME(RUNNING);
    _STATE_NAME(STOP_PENDING);
    _STATE_NAME(STOPPED);
    default:
        break;
    }

    return "UNKNOWN";

#undef  _STATE_NAME
}

static VOID
ReportStatus(
    IN  DWORD           CurrentState,
    IN  DWORD           Win32ExitCode,
    IN  DWORD           WaitHint)
{
    PMONITOR_CONTEXT    Context = &MonitorContext;
    static DWORD        CheckPoint = 1;
    BOOL                Success;
    HRESULT             Error;

    Log("====> (%s)", ServiceStateName(CurrentState));

    Context->Status.dwCurrentState = CurrentState;
    Context->Status.dwWin32ExitCode = Win32ExitCode;
    Context->Status.dwWaitHint = WaitHint;

    if (CurrentState == SERVICE_START_PENDING)
        Context->Status.dwControlsAccepted = 0;
    else
        Context->Status.dwControlsAccepted = SERVICE_ACCEPT_STOP |
                                             SERVICE_ACCEPT_SHUTDOWN |
                                             SERVICE_ACCEPT_SESSIONCHANGE;

    if (CurrentState == SERVICE_RUNNING ||
        CurrentState == SERVICE_STOPPED )
        Context->Status.dwCheckPoint = 0;
    else
        Context->Status.dwCheckPoint = CheckPoint++;

    Success = SetServiceStatus(Context->Service, &Context->Status);

    if (!Success)
        goto fail1;

    Log("<====");

    return;

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;
        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }
}

DWORD WINAPI
MonitorCtrlHandlerEx(
    IN  DWORD           Ctrl,
    IN  DWORD           EventType,
    IN  LPVOID          EventData,
    IN  LPVOID          Argument
    )
{
    PMONITOR_CONTEXT    Context = &MonitorContext;

    UNREFERENCED_PARAMETER(EventType);
    UNREFERENCED_PARAMETER(EventData);
    UNREFERENCED_PARAMETER(Argument);

    switch (Ctrl) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        ReportStatus(SERVICE_STOP_PENDING, NO_ERROR, 0);
        SetEvent(Context->StopEvent);
        return NO_ERROR;

    case SERVICE_CONTROL_SESSIONCHANGE:
        SetEvent(Context->RequestEvent);
        return NO_ERROR;

    case SERVICE_CONTROL_INTERROGATE:
        ReportStatus(SERVICE_RUNNING, NO_ERROR, 0);
        return NO_ERROR;

    default:
        break;
    }

    ReportStatus(SERVICE_RUNNING, NO_ERROR, 0);
    return ERROR_CALL_NOT_IMPLEMENTED;
}

static const CHAR *
WTSStateName(
    IN  DWORD   State
    )
{
#define _STATE_NAME(_State) \
    case WTS ## _State: \
        return #_State

    switch (State) {
    _STATE_NAME(Active);
    _STATE_NAME(Connected);
    _STATE_NAME(ConnectQuery);
    _STATE_NAME(Shadow);
    _STATE_NAME(Disconnected);
    _STATE_NAME(Idle);
    _STATE_NAME(Listen);
    _STATE_NAME(Reset);
    _STATE_NAME(Down);
    _STATE_NAME(Init);
    default:
        break;
    }

    return "UNKNOWN";

#undef  _STATE_NAME
}

static VOID
DoReboot(
    VOID
    )
{
    (VOID) InitiateSystemShutdownEx(NULL,
                                    NULL,
                                    0,
                                    TRUE,
                                    TRUE,
                                    SHTDN_REASON_MAJOR_OPERATINGSYSTEM |
                                    SHTDN_REASON_MINOR_INSTALLATION |
                                    SHTDN_REASON_FLAG_PLANNED);
}

static VOID
PromptForReboot(
    IN PTCHAR           DriverName
    )
{
    PMONITOR_CONTEXT    Context = &MonitorContext;
    HRESULT             Result;
    TCHAR               ServiceKeyName[MAX_PATH];
    HKEY                ServiceKey;
    DWORD               MaxValueLength;
    DWORD               DisplayNameLength;
    PTCHAR              DisplayName;
    PTCHAR              Description;
    DWORD               Type;
    TCHAR               Title[] = TEXT(VENDOR_NAME_STR);
    TCHAR               Message[MAXIMUM_BUFFER_SIZE];
    DWORD               Length;
    PWTS_SESSION_INFO   SessionInfo;
    DWORD               Count;
    DWORD               Index;
    BOOL                Success;
    HRESULT             Error;

    Log("====> (%s)", DriverName);

    Result = StringCbPrintf(ServiceKeyName,
                            MAX_PATH,
                            SERVICES_KEY "\\%s",
                            DriverName);
    assert(SUCCEEDED(Result));

    Error = RegOpenKeyEx(HKEY_LOCAL_MACHINE,
                         ServiceKeyName,
                         0,
                         KEY_READ,
                         &ServiceKey);
    if (Error != ERROR_SUCCESS) {
        SetLastError(Error);
        goto fail1;
    }

    Error = RegQueryInfoKey(ServiceKey,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            &MaxValueLength,
                            NULL,
                            NULL);
    if (Error != ERROR_SUCCESS) {
        SetLastError(Error);
        goto fail2;
    }

    DisplayNameLength = MaxValueLength + sizeof (TCHAR);

    DisplayName = calloc(1, DisplayNameLength);
    if (DisplayName == NULL)
        goto fail3;

    Error = RegQueryValueEx(ServiceKey,
                            "DisplayName",
                            NULL,
                            &Type,
                            (LPBYTE)DisplayName,
                            &DisplayNameLength);
    if (Error != ERROR_SUCCESS) {
        SetLastError(Error);
        goto fail4;
    }

    if (Type != REG_SZ) {
        SetLastError(ERROR_BAD_FORMAT);
        goto fail5;
    }

    Description = _tcsrchr(DisplayName, ';');
    if (Description == NULL)
        Description = DisplayName;
    else
        Description++;

    Result = StringCbPrintf(Message,
                            MAXIMUM_BUFFER_SIZE,
                            TEXT("%s "),
                            Description);
    assert(SUCCEEDED(Result));

    Length = (DWORD)_tcslen(Message);

    Length = LoadString(GetModuleHandle(NULL),
                        IDS_DIALOG,
                        Message + Length,
                        ARRAYSIZE(Message) - Length);
    if (Length == 0)
        goto fail6;

    Success = WTSEnumerateSessions(WTS_CURRENT_SERVER_HANDLE,
                                   0,
                                   1,
                                   &SessionInfo,
                                   &Count);

    if (!Success)
        goto fail7;

    for (Index = 0; Index < Count; Index++) {
        DWORD                   SessionId = SessionInfo[Index].SessionId;
        PTCHAR                  Name = SessionInfo[Index].pWinStationName;
        WTS_CONNECTSTATE_CLASS  State = SessionInfo[Index].State;
        DWORD                   Response;

        Log("[%u]: %s [%s]",
            SessionId,
            Name,
            WTSStateName(State));

        if (State != WTSActive)
            continue;

        Success = WTSSendMessage(WTS_CURRENT_SERVER_HANDLE,
                                 SessionId,
                                 Title,
                                 sizeof (Title),
                                 Message,
                                 sizeof (Message),
                                 MB_YESNO | MB_ICONEXCLAMATION,
                                 0,
                                 &Response,
                                 TRUE);

        if (!Success)
            goto fail8;

        Context->RebootPending = TRUE;

        if (Response == IDYES)
            DoReboot();

        break;
    }

    WTSFreeMemory(SessionInfo);

    free(DisplayName);

    RegCloseKey(ServiceKey);

    Log("<====");

    return;

fail8:
    Log("fail8");

    WTSFreeMemory(SessionInfo);

fail7:
    Log("fail7");

fail6:
    Log("fail6");

fail5:
    Log("fail5");

fail4:
    Log("fail4");

    free(DisplayName);

fail3:
    Log("fail3");

fail2:
    Log("fail2");

    RegCloseKey(ServiceKey);

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;
        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }
}

static VOID
CheckRebootValue(
    VOID
    )
{
    PMONITOR_CONTEXT    Context = &MonitorContext;
    HRESULT             Error;
    DWORD               MaxValueLength;
    DWORD               RebootLength;
    PTCHAR              Reboot;
    DWORD               Type;

    Log("====>");

    Error = RegQueryInfoKey(Context->RequestKey,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            &MaxValueLength,
                            NULL,
                            NULL);
    if (Error != ERROR_SUCCESS) {
        SetLastError(Error);
        goto fail1;
    }

    RebootLength = MaxValueLength + sizeof (TCHAR);

    Reboot = calloc(1, RebootLength);
    if (Reboot == NULL)
        goto fail2;

    Error = RegQueryValueEx(Context->RequestKey,
                            "Reboot",
                            NULL,
                            &Type,
                            (LPBYTE)Reboot,
                            &RebootLength);
    if (Error != ERROR_SUCCESS) {
        if (Error == ERROR_FILE_NOT_FOUND)
            goto done;

        SetLastError(Error);
        goto fail3;
    }

    if (Type != REG_SZ) {
        SetLastError(ERROR_BAD_FORMAT);
        goto fail4;
    }

    if (!Context->RebootPending)
        PromptForReboot(Reboot);

    if (Context->RebootPending)
        (VOID) RegDeleteValue(Context->RequestKey, "Reboot");

done:
    free(Reboot);

    Log("<====");

    return;

fail4:
    Log("fail4");

fail3:
    Log("fail3");

    free(Reboot);

fail2:
    Log("fail2");

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;
        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }
}

static VOID
CheckRequestKey(
    VOID
    )
{
    PMONITOR_CONTEXT    Context = &MonitorContext;
    HRESULT             Error;

    Log("====>");

    CheckRebootValue();

    Error = RegNotifyChangeKeyValue(Context->RequestKey,
                                    TRUE,
                                    REG_NOTIFY_CHANGE_LAST_SET,
                                    Context->RequestEvent,
                                    TRUE);

    if (Error != ERROR_SUCCESS)
        goto fail1;

    Log("<====");

    return;

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;
        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }
}

static BOOL
AcquireShutdownPrivilege(
    VOID
    )
{
    HANDLE              Token;
    TOKEN_PRIVILEGES    New;
    BOOL                Success;
    HRESULT             Error;

    Log("====>");

    New.PrivilegeCount = 1;

    Success = LookupPrivilegeValue(NULL,
                                   SE_SHUTDOWN_NAME,
                                   &New.Privileges[0].Luid);

    if (!Success)
        goto fail1;

    New.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    Success = OpenProcessToken(GetCurrentProcess(),
                               TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
                               &Token);

    if (!Success)
        goto fail2;

    Success = AdjustTokenPrivileges(Token,
                                    FALSE,
                                    &New,
                                    0,
                                    NULL,
                                    NULL);

    if (!Success)
        goto fail3;

    CloseHandle(Token);

    Log("<====");

    return TRUE;

fail3:
    Log("fail3");

    CloseHandle(Token);

fail2:
    Log("fail2");

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;
        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return FALSE;
}

VOID WINAPI
MonitorMain(
    _In_    DWORD       argc,
    _In_    LPTSTR      *argv
    )
{
    PMONITOR_CONTEXT    Context = &MonitorContext;
    BOOL                Success;
    HRESULT             Error;

    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    Log("====>");

    Success = AcquireShutdownPrivilege();

    if (!Success)
        goto fail1;

    Context->Service = RegisterServiceCtrlHandlerEx(MONITOR_NAME,
                                                    MonitorCtrlHandlerEx,
                                                    NULL);
    if (Context->Service == NULL)
        goto fail2;

    Context->EventLog = RegisterEventSource(NULL,
                                            MONITOR_NAME);
    if (Context->EventLog == NULL)
        goto fail3;

    Context->Status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    Context->Status.dwServiceSpecificExitCode = 0;

    ReportStatus(SERVICE_START_PENDING, NO_ERROR, 3000);

    Context->StopEvent = CreateEvent(NULL,
                                     TRUE,
                                     FALSE,
                                     NULL);

    if (Context->StopEvent == NULL)
        goto fail4;

    Context->RequestEvent = CreateEvent(NULL,
                                        TRUE,
                                        FALSE,
                                        NULL);

    if (Context->RequestEvent == NULL)
        goto fail5;

    Error = RegOpenKeyEx(HKEY_LOCAL_MACHINE,
                         REQUEST_KEY,
                         0,
                         KEY_ALL_ACCESS,
                         &Context->RequestKey);

    if (Error != ERROR_SUCCESS)
        goto fail6;

    SetEvent(Context->RequestEvent);

    ReportStatus(SERVICE_RUNNING, NO_ERROR, 0);

    for (;;) {
        HANDLE  Events[2];
        DWORD   Object;

        Events[0] = Context->StopEvent;
        Events[1] = Context->RequestEvent;

        Log("waiting (%u)...", ARRAYSIZE(Events));
        Object = WaitForMultipleObjects(ARRAYSIZE(Events),
                                        Events,
                                        FALSE,
                                        INFINITE);
        Log("awake");

        switch (Object) {
        case WAIT_OBJECT_0:
            ResetEvent(Events[0]);
            goto done;

        case WAIT_OBJECT_0 + 1:
            ResetEvent(Events[1]);
            CheckRequestKey();
            break;

        default:
            break;
        }
    }

done:
    CloseHandle(Context->RequestKey);
    CloseHandle(Context->RequestEvent);
    CloseHandle(Context->StopEvent);

    ReportStatus(SERVICE_STOPPED, NO_ERROR, 0);

    (VOID) DeregisterEventSource(Context->EventLog);

    Log("<====");

    return;

fail6:
    Log("fail6");

    ReportStatus(SERVICE_STOPPED, GetLastError(), 0);

    CloseHandle(Context->RequestEvent);

fail5:
    Log("fail5");

    CloseHandle(Context->StopEvent);

fail4:
    Log("fail4");

    (VOID) DeregisterEventSource(Context->EventLog);

fail3:
    Log("fail3");

fail2:
    Log("fail2");

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;
        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }
}

static BOOL
MonitorCreate(
    VOID
    )
{
    SC_HANDLE   SCManager;
    SC_HANDLE   Service;
    TCHAR       Path[MAX_PATH];
    HRESULT     Error;

    Log("====>");

    if(!GetModuleFileName(NULL, Path, MAX_PATH))
        goto fail1;

    SCManager = OpenSCManager(NULL,
                              NULL,
                              SC_MANAGER_ALL_ACCESS);

    if (SCManager == NULL)
        goto fail2;

    Service = CreateService(SCManager,
                            MONITOR_NAME,
                            MONITOR_DISPLAYNAME,
                            SERVICE_ALL_ACCESS,
                            SERVICE_WIN32_OWN_PROCESS,
                            SERVICE_AUTO_START,
                            SERVICE_ERROR_NORMAL,
                            Path,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            NULL);

    if (Service == NULL)
        goto fail3;

    CloseServiceHandle(Service);
    CloseServiceHandle(SCManager);

    Log("<====");

    return TRUE;

fail3:
    Log("fail3");

    CloseServiceHandle(SCManager);

fail2:
    Log("fail2");

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;
        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return FALSE;
}

static BOOL
MonitorDelete(
    VOID
    )
{
    SC_HANDLE           SCManager;
    SC_HANDLE           Service;
    BOOL                Success;
    SERVICE_STATUS      Status;
    HRESULT             Error;

    Log("====>");

    SCManager = OpenSCManager(NULL,
                              NULL,
                              SC_MANAGER_ALL_ACCESS);

    if (SCManager == NULL)
        goto fail1;

    Service = OpenService(SCManager,
                          MONITOR_NAME,
                          SERVICE_ALL_ACCESS);

    if (Service == NULL)
        goto fail2;

    Success = ControlService(Service,
                             SERVICE_CONTROL_STOP,
                             &Status);

    if (!Success)
        goto fail3;

    Success = DeleteService(Service);

    if (!Success)
        goto fail4;

    CloseServiceHandle(Service);
    CloseServiceHandle(SCManager);

    Log("<====");

    return TRUE;

fail4:
    Log("fail4");

fail3:
    Log("fail3");

    CloseServiceHandle(Service);

fail2:
    Log("fail2");

    CloseServiceHandle(SCManager);

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;
        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return FALSE;
}

static BOOL
MonitorEntry(
    VOID
    )
{
    SERVICE_TABLE_ENTRY Table[] = {
        { MONITOR_NAME, MonitorMain },
        { NULL, NULL }
    };
    HRESULT             Error;

    Log("%s (%s) ====>",
        MAJOR_VERSION_STR "." MINOR_VERSION_STR "." MICRO_VERSION_STR "." BUILD_NUMBER_STR,
        DAY_STR "/" MONTH_STR "/" YEAR_STR);

    if (!StartServiceCtrlDispatcher(Table))
        goto fail1;

    Log("%s (%s) <====",
        MAJOR_VERSION_STR "." MINOR_VERSION_STR "." MICRO_VERSION_STR "." BUILD_NUMBER_STR,
        DAY_STR "/" MONTH_STR "/" YEAR_STR);

    return TRUE;

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;
        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return FALSE;
}

int CALLBACK
_tWinMain(
    _In_        HINSTANCE   Current,
    _In_opt_    HINSTANCE   Previous,
    _In_        LPSTR       CmdLine,
    _In_        int         CmdShow
    )
{
    BOOL                    Success;

    UNREFERENCED_PARAMETER(Current);
    UNREFERENCED_PARAMETER(Previous);
    UNREFERENCED_PARAMETER(CmdShow);

    if (_tcslen(CmdLine) != 0) {
         if (_tcsicmp(CmdLine, TEXT("create")) == 0)
             Success = MonitorCreate();
         else if (_tcsicmp(CmdLine, TEXT("delete")) == 0)
             Success = MonitorDelete();
         else
             Success = FALSE;
    } else
        Success = MonitorEntry();

    return Success ? 0 : 1;
}
