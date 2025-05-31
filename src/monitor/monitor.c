/* Copyright (c) Xen Project.
 * Copyright (c) Cloud Software Group, Inc.
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
#include <cfgmgr32.h>
#include <winternl.h>
#include <powrprof.h>
#include <malloc.h>
#include <assert.h>

#include <version.h>

#include "messages.h"

#define stringify_literal(_text) #_text
#define stringify(_text) stringify_literal(_text)
#define __MODULE__ stringify(PROJECT)

#define MONITOR_NAME        __MODULE__
#define MONITOR_DISPLAYNAME MONITOR_NAME

typedef struct _MONITOR_CONTEXT {
    SERVICE_STATUS          Status;
    SERVICE_STATUS_HANDLE   Service;
    HKEY                    ParametersKey;
    HANDLE                  EventLog;
    HANDLE                  StopEvent;
    HANDLE                  RequestEvent;
    HANDLE                  Timer;
    HKEY                    RequestKey;
    PTSTR                   Title;
    PTSTR                   Text;
    PTSTR                   Question;
    BOOL                    RebootPrompted;
    PTSTR                   RebootRequestedBy;
    HANDLE                  ResponseEvent;
    DWORD                   Response;
} MONITOR_CONTEXT, *PMONITOR_CONTEXT;

typedef struct _REBOOT_PROMPT {
    PTSTR                   Title;
    PTSTR                   Text;
    HANDLE                  ResponseEvent;
    PDWORD                  PResponse;
} REBOOT_PROMPT, *PREBOOT_PROMPT;

MONITOR_CONTEXT MonitorContext;

#define MAXIMUM_BUFFER_SIZE 1024
#define REBOOT_RETRY_DELAY  60000L // 1 minute

#define SERVICES_KEY "SYSTEM\\CurrentControlSet\\Services"

#define SERVICE_KEY(_Service) \
        SERVICES_KEY ## "\\" ## _Service

#define PARAMETERS_KEY(_Service) \
        SERVICE_KEY(_Service) ## "\\Parameters"

static VOID
#pragma prefast(suppress:6262) // Function uses '1036' bytes of stack: exceeds /analyze:stacksize'1024'
__Log(
    _In_ PCSTR          Format,
    ...
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

    _Analysis_assume_(Length < MAXIMUM_BUFFER_SIZE);
    _Analysis_assume_(Length >= 2);
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

static PTSTR
GetErrorMessage(
    _In_  HRESULT   Error
    )
{
    PTSTR           Message;
    ULONG           Index;

    if (!FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                       FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL,
                       Error,
                       MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                       (LPTSTR)&Message,
                       0,
                       NULL))
        return NULL;

    for (Index = 0; Message[Index] != '\0'; Index++) {
        if (Message[Index] == '\r' || Message[Index] == '\n') {
            Message[Index] = '\0';
            break;
        }
    }

    return Message;
}

static PCSTR
ServiceStateName(
    _In_ DWORD  State
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
    _In_ DWORD          CurrentState,
    _In_ DWORD          Win32ExitCode,
    _In_ DWORD          WaitHint)
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
        PTSTR   Message;
        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }
}

DWORD WINAPI
MonitorCtrlHandlerEx(
    _In_ DWORD          Ctrl,
    _In_ DWORD          EventType,
    _In_ LPVOID         EventData,
    _In_ LPVOID         Argument
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

static PCSTR
WTSStateName(
    _In_ DWORD  State
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
    _In_ PTSTR  Message,
    _In_ DWORD  Timeout
    )
{
    Log("waiting for pending install events...");

    (VOID) CM_WaitNoPendingInstallEvents(INFINITE);

    Log("initiating shutdown...");

#pragma prefast(suppress:28159)
    (VOID) InitiateSystemShutdownEx(NULL,
                                    Message,
                                    Timeout,
                                    TRUE,
                                    TRUE,
                                    SHTDN_REASON_MAJOR_OPERATINGSYSTEM |
                                    SHTDN_REASON_MINOR_INSTALLATION |
                                    SHTDN_REASON_FLAG_PLANNED);
}

static DWORD
GetPromptTimeout(
    VOID
    )
{
    PMONITOR_CONTEXT    Context = &MonitorContext;
    DWORD               Type;
    DWORD               Value;
    DWORD               ValueLength;
    HRESULT             Error;

    ValueLength = sizeof (Value);

    Error = RegQueryValueEx(Context->ParametersKey,
                            "PromptTimeout",
                            NULL,
                            &Type,
                            (LPBYTE)&Value,
                            &ValueLength);
    if (Error != ERROR_SUCCESS ||
        Type != REG_DWORD)
        Value = 0;

    Log("%u", Value);

    return Value;
}

static PTSTR
GetDisplayName(
    _In_ PTSTR          DriverName
    )
{
    HRESULT             Result;
    TCHAR               ServiceKeyName[MAX_PATH];
    HKEY                ServiceKey;
    DWORD               MaxValueLength;
    DWORD               Type;
    DWORD               DisplayNameLength;
    PTSTR               DisplayName;
    HRESULT             Error;

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

    RegCloseKey(ServiceKey);

    return DisplayName;

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
        PTSTR   Message;
        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return NULL;
}

static VOID
RebootPromptFree(
    PREBOOT_PROMPT      Prompt
    )
{
    if (Prompt) {
        free(Prompt->Text);
        free(Prompt->Title);
        free(Prompt);
    }
}

static DWORD WINAPI
DoPromptForReboot(
    LPVOID lpThreadParameter
    )
{
    PREBOOT_PROMPT      Prompt = lpThreadParameter;
    DWORD               TitleLength;
    DWORD               TextLength;
    DWORD               Timeout;
    PWTS_SESSION_INFO   SessionInfo;
    DWORD               Count;
    DWORD               Index;
    BOOL                Success;
    DWORD               Error;

    assert(Prompt);
    assert(Prompt->ResponseEvent && Prompt->PResponse);
    assert(Prompt->Title && Prompt->Text);

    Error = ERROR_SUCCESS;

    TitleLength = (DWORD)((_tcslen(Prompt->Title) +
                           1) * sizeof (TCHAR));
    TextLength = (DWORD)((_tcslen(Prompt->Text) +
                           1) * sizeof (TCHAR));

    Success = WTSEnumerateSessions(WTS_CURRENT_SERVER_HANDLE,
                                   0,
                                   1,
                                   &SessionInfo,
                                   &Count);
    if (!Success) {
        Error = GetLastError();
        goto fail1;
    }

    Timeout = GetPromptTimeout();

    *Prompt->PResponse = 0;

    for (Index = 0; Index < Count; Index++) {
        DWORD                   SessionId = SessionInfo[Index].SessionId;
        PTSTR                   Name = SessionInfo[Index].pWinStationName;
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
                                 Prompt->Title,
                                 TitleLength,
                                 Prompt->Text,
                                 TextLength,
                                 MB_YESNO | MB_ICONEXCLAMATION,
                                 Timeout,
                                 &Response,
                                 TRUE);

        if (!Success)
            goto fail2;

        *Prompt->PResponse = Response;
        (VOID) SetEvent(Prompt->ResponseEvent);

        break;
    }

    WTSFreeMemory(SessionInfo);
    RebootPromptFree(Prompt);

    return ERROR_SUCCESS;

fail2:
    Log("fail2");
    *Prompt->PResponse = 0;

fail1:
    RebootPromptFree(Prompt);

    return Error;
}

static VOID
PromptForReboot(
    _In_ PTSTR          DriverName
    )
{
    PMONITOR_CONTEXT    Context = &MonitorContext;
    HRESULT             Result;
    PREBOOT_PROMPT      Prompt;
    PTSTR               DisplayName;
    PTSTR               Description;
    HANDLE              PromptThread;
    DWORD               TextLength;
    DWORD               Error;

    assert(DriverName);

    /*
     * Can't use Context->Response here since a previous prompt may not have
     * gotten a response.
     */
    if (Context->RebootPrompted)
        return;
    Context->RebootPrompted = TRUE;

    Log("====> (%s)", DriverName);

    Prompt = calloc(1, sizeof (REBOOT_PROMPT));
    if (Prompt == NULL) {
        Error = ERROR_OUTOFMEMORY;
        goto fail1;
    }
    Prompt->ResponseEvent = Context->ResponseEvent;
    Prompt->PResponse = &Context->Response;

    Prompt->Title = _tcsdup(Context->Title);
    if (Prompt->Title == NULL) {
        Error = ERROR_OUTOFMEMORY;
        goto fail2;
    }

    DisplayName = GetDisplayName(DriverName);
    if (DisplayName == NULL) {
        Error = GetLastError();
        goto fail3;
    }

    Description = _tcsrchr(DisplayName, ';');
    if (Description == NULL)
        Description = DisplayName;
    else
        Description++;

    TextLength = (DWORD)((_tcslen(Description) +
                          1 + // ' '
                          _tcslen(Context->Text) +
                          1 + // ' '
                          _tcslen(Context->Question) +
                          1) * sizeof (TCHAR));

    Prompt->Text = calloc(1, TextLength);
    if (Prompt->Text == NULL) {
        Error = ERROR_OUTOFMEMORY;
        goto fail4;
    }

    Result = StringCbPrintf(Prompt->Text,
                            TextLength,
                            TEXT("%s %s %s"),
                            Description,
                            Context->Text,
                            Context->Question);
    assert(SUCCEEDED(Result));

    PromptThread = CreateThread(NULL,
                                0,
                                &DoPromptForReboot,
                                Prompt,
                                0,
                                NULL);
    if (PromptThread == NULL) {
        Error = GetLastError();
        goto fail4;
    }

    CloseHandle(PromptThread);
    free(DisplayName);
    // ownership of Prompt handed to prompt thread

    return;

fail4:
    Log("fail4");
    free(DisplayName);

fail3:
    Log("fail3");

fail2:
    Log("fail2");

fail1:
    {
        PTSTR   Message;
        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    RebootPromptFree(Prompt);
}

static VOID
TryAutoReboot(
    _In_ PTSTR          DriverName
    )
{
    PMONITOR_CONTEXT    Context = &MonitorContext;
    HRESULT             Result;
    HANDLE              MsiMutex;
    DWORD               Type;
    DWORD               AutoReboot;
    DWORD               RebootCount;
    DWORD               Length;
    DWORD               Timeout;
    PTSTR               DisplayName;
    PTSTR               Description;
    PTSTR               Text;
    DWORD               TextLength;
    ULONG               PowerInfo;
    NTSTATUS            Status;
    DWORD               Error;

    if (!Context->RebootRequestedBy) {
        Context->RebootRequestedBy = _tcsdup(DriverName);
        if (!Context->RebootRequestedBy) {
            Error = ERROR_OUTOFMEMORY;
            goto fail1;
        }
    }

    // We don't want to suddenly reboot if the user's already said no.
    if (Context->Response == IDNO)
        goto done;

    // Check if there's an installation under way.
    MsiMutex = OpenMutex(SYNCHRONIZE,
                         FALSE,
                         TEXT("Global\\_MSIExecute"));
    if (MsiMutex != NULL) {
        Error = WaitForSingleObject(MsiMutex, 0);
        if (Error == WAIT_OBJECT_0 || Error == WAIT_ABANDONED)
            ReleaseMutex(MsiMutex);

        CloseHandle(MsiMutex);

        if (Error == WAIT_TIMEOUT)
            // The only case where an installation is definitely running.
            goto done;
    }

    Status = CallNtPowerInformation(SystemExecutionState,
                                    NULL,
                                    0,
                                    &PowerInfo,
                                    sizeof(PowerInfo));
    if (Status < 0 || (PowerInfo & ES_SYSTEM_REQUIRED))
        goto done;

    Length = sizeof (DWORD);

    Error = RegQueryValueEx(Context->ParametersKey,
                            "AutoReboot",
                            NULL,
                            &Type,
                            (LPBYTE)&AutoReboot,
                            &Length);
    if (Error != ERROR_SUCCESS ||
        Type != REG_DWORD)
        AutoReboot = 0;

    if (AutoReboot == 0)
        goto prompt;

    Length = sizeof (DWORD);

    Error = RegQueryValueEx(Context->ParametersKey,
                            "RebootCount",
                            NULL,
                            &Type,
                            (LPBYTE)&RebootCount,
                            &Length);
    if (Error != ERROR_SUCCESS ||
        Type != REG_DWORD)
        RebootCount = 0;

    if (RebootCount >= AutoReboot)
        goto prompt;

    Log("AutoRebooting (reboot %u of %u)\n",
        RebootCount,
        AutoReboot);

    ++RebootCount;

    (VOID) RegSetValueEx(Context->ParametersKey,
                         "RebootCount",
                         0,
                         REG_DWORD,
                         (const BYTE*)&RebootCount,
                         (DWORD) sizeof(DWORD));

    (VOID) RegFlushKey(Context->ParametersKey);

    Error = RegQueryValueEx(Context->ParametersKey,
                            "AutoRebootTimeout",
                            NULL,
                            &Type,
                            (LPBYTE)&Timeout,
                            &Length);
    if (Error != ERROR_SUCCESS ||
        Type != REG_DWORD)
        Timeout = 60;

    DisplayName = GetDisplayName(Context->RebootRequestedBy);
    if (DisplayName == NULL) {
        Error = GetLastError();
        goto fail2;
    }

    Description = _tcsrchr(DisplayName, ';');
    if (Description == NULL)
        Description = DisplayName;
    else
        Description++;

    TextLength = (DWORD)((_tcslen(Description) +
                          1 + // ' '
                          _tcslen(Context->Text) +
                          1) * sizeof (TCHAR));

    Text = calloc(1, TextLength);
    if (Text == NULL) {
        SetLastError(ERROR_OUTOFMEMORY);
        goto fail3;
    }

    Result = StringCbPrintf(Text,
                            TextLength,
                            TEXT("%s %s"),
                            Description,
                            Context->Text);
    assert(SUCCEEDED(Result));

    free(DisplayName);

    DoReboot(Text, Timeout);

    free(Text);

    return;

prompt:
    PromptForReboot(Context->RebootRequestedBy);

    return;

done:
    return;

fail3:
    Log("fail3");

    free(DisplayName);

fail2:
    Log("fail2");

fail1:
    {
        PTSTR   Message;
        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return;
}

static VOID
CheckRequestSubKeys(
    VOID
    )
{
    PMONITOR_CONTEXT    Context = &MonitorContext;
    DWORD               SubKeys;
    DWORD               MaxSubKeyLength;
    DWORD               SubKeyLength;
    PTSTR               SubKeyName;
    DWORD               Index;
    HKEY                SubKey;
    HRESULT             Error;

    Log("====>");

    Error = RegQueryInfoKey(Context->RequestKey,
                            NULL,
                            NULL,
                            NULL,
                            &SubKeys,
                            &MaxSubKeyLength,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            NULL);
    if (Error != ERROR_SUCCESS) {
        SetLastError(Error);
        goto fail1;
    }

    SubKeyLength = MaxSubKeyLength + sizeof (TCHAR);

    SubKeyName = calloc(1, SubKeyLength);
    if (SubKeyName == NULL)
        goto fail2;

    for (Index = 0; Index < SubKeys; Index++) {
        DWORD   Length;
        DWORD   Type;
        DWORD   Reboot;

        SubKeyLength = MaxSubKeyLength + sizeof (TCHAR);
        memset(SubKeyName, 0, SubKeyLength);

        Error = RegEnumKeyEx(Context->RequestKey,
                             Index,
                             (LPTSTR)SubKeyName,
                             &SubKeyLength,
                             NULL,
                             NULL,
                             NULL,
                             NULL);
        if (Error != ERROR_SUCCESS) {
            SetLastError(Error);
            goto fail3;
        }

        Log("%s", SubKeyName);

        Error = RegOpenKeyEx(Context->RequestKey,
                             SubKeyName,
                             0,
                             KEY_READ,
                             &SubKey);
        if (Error != ERROR_SUCCESS)
            continue;

        Length = sizeof (DWORD);
        Error = RegQueryValueEx(SubKey,
                                "Reboot",
                                NULL,
                                &Type,
                                (LPBYTE)&Reboot,
                                &Length);
        if (Error != ERROR_SUCCESS ||
            Type != REG_DWORD)
            goto loop;

        if (Reboot != 0)
            goto found;

loop:
        RegCloseKey(SubKey);
    }

    Error = RegDeleteValue(Context->ParametersKey,
                           "RebootCount");
    if (Error == ERROR_SUCCESS)
        (VOID) RegFlushKey(Context->ParametersKey);

    goto done;

found:
    RegCloseKey(SubKey);

    if (!Context->RebootRequestedBy)
        TryAutoReboot(SubKeyName);

done:
    free(SubKeyName);

    Log("<====");

    return;

fail3:
    Log("fail3");

    free(SubKeyName);

fail2:
    Log("fail2");

fail1:
    Error = GetLastError();

    {
        PTSTR   Message;
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

    CheckRequestSubKeys();

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
        PTSTR   Message;
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
        PTSTR   Message;
        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return FALSE;
}

_Success_(return)
static BOOL
GetRequestKeyName(
    _Outptr_result_z_ PTSTR     *RequestKeyName
    )
{
    PMONITOR_CONTEXT            Context = &MonitorContext;
    DWORD                       MaxValueLength;
    DWORD                       RequestKeyNameLength;
    DWORD                       Type;
    HRESULT                     Error;

    Error = RegQueryInfoKey(Context->ParametersKey,
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

    RequestKeyNameLength = MaxValueLength + sizeof (TCHAR);

    *RequestKeyName = calloc(1, RequestKeyNameLength);
    if (*RequestKeyName == NULL)
        goto fail2;

    Error = RegQueryValueEx(Context->ParametersKey,
                            "RequestKey",
                            NULL,
                            &Type,
                            (LPBYTE)(*RequestKeyName),
                            &RequestKeyNameLength);
    if (Error != ERROR_SUCCESS) {
        SetLastError(Error);
        goto fail3;
    }

    if (Type != REG_SZ) {
        SetLastError(ERROR_BAD_FORMAT);
        goto fail4;
    }

    Log("%s", *RequestKeyName);

    return TRUE;

fail4:
    Log("fail4");

fail3:
    Log("fail3");

    free(*RequestKeyName);

fail2:
    Log("fail2");

fail1:
    Error = GetLastError();

    {
        PTSTR   Message;
        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return FALSE;
}

static BOOL
GetDialogParameters(
    VOID
    )
{
    PMONITOR_CONTEXT    Context = &MonitorContext;
    DWORD               MaxValueLength;
    DWORD               TitleLength;
    DWORD               TextLength;
    DWORD               QuestionLength;
    DWORD               Type;
    HRESULT             Error;

    Error = RegQueryInfoKey(Context->ParametersKey,
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

    TitleLength = MaxValueLength + sizeof (TCHAR);

    Context->Title = calloc(1, TitleLength);
    if (Context == NULL)
        goto fail2;

    Error = RegQueryValueEx(Context->ParametersKey,
                            "DialogTitle",
                            NULL,
                            &Type,
                            (LPBYTE)Context->Title,
                            &TitleLength);
    if (Error != ERROR_SUCCESS) {
        SetLastError(Error);
        goto fail3;
    }

    if (Type != REG_SZ) {
        SetLastError(ERROR_BAD_FORMAT);
        goto fail4;
    }

    TextLength = MaxValueLength + sizeof (TCHAR);

    Context->Text = calloc(1, TextLength);
    if (Context == NULL)
        goto fail5;

    Error = RegQueryValueEx(Context->ParametersKey,
                            "DialogText",
                            NULL,
                            &Type,
                            (LPBYTE)Context->Text,
                            &TextLength);
    if (Error != ERROR_SUCCESS) {
        SetLastError(Error);
        goto fail6;
    }

    if (Type != REG_SZ) {
        SetLastError(ERROR_BAD_FORMAT);
        goto fail7;
    }

    QuestionLength = MaxValueLength + sizeof (TCHAR);

    Context->Question = calloc(1, QuestionLength);
    if (Context == NULL)
        goto fail8;

    Error = RegQueryValueEx(Context->ParametersKey,
                            "DialogQuestion",
                            NULL,
                            &Type,
                            (LPBYTE)Context->Question,
                            &QuestionLength);
    if (Error != ERROR_SUCCESS) {
        SetLastError(Error);
        goto fail9;
    }

    if (Type != REG_SZ) {
        SetLastError(ERROR_BAD_FORMAT);
        goto fail10;
    }

    return TRUE;

fail10:
    Log("fail10");

fail9:
    Log("fail9");

    free(Context->Question);

fail8:
    Log("fail8");

fail7:
    Log("fail7");

fail6:
    Log("fail6");

    free(Context->Text);

fail5:
    Log("fail5");

fail4:
    Log("fail4");

fail3:
    Log("fail3");

    free(Context->Title);

fail2:
    Log("fail2");

fail1:
    Error = GetLastError();

    {
        PTSTR   Message;
        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return FALSE;
}

static BOOL
RemoveStartOverride(
    _In_ PTSTR          DriverName
    )
{
    TCHAR               KeyName[MAX_PATH];
    HRESULT             Error;

    Error = StringCbPrintf(KeyName,
                           MAX_PATH,
                           SERVICES_KEY "\\%s\\StartOverride",
                           DriverName);
    assert(SUCCEEDED(Error));

    Error = RegDeleteKey(HKEY_LOCAL_MACHINE, KeyName);
    if (Error != ERROR_SUCCESS)
        goto fail1;

    return TRUE;

fail1:
    Error = GetLastError();

    {
        PTSTR   Message;
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
    PTSTR               RequestKeyName;
    BOOL                Success;
    HRESULT             Error;
    LARGE_INTEGER       DueTime;

    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    Log("====>");

    (VOID) RemoveStartOverride("stornvme");

    Error = RegOpenKeyEx(HKEY_LOCAL_MACHINE,
                         PARAMETERS_KEY(__MODULE__),
                         0,
                         KEY_READ,
                         &Context->ParametersKey);
    if (Error != ERROR_SUCCESS)
        goto fail1;

    Success = AcquireShutdownPrivilege();
    if (!Success)
        goto fail2;

    Context->Service = RegisterServiceCtrlHandlerEx(MONITOR_NAME,
                                                    MonitorCtrlHandlerEx,
                                                    NULL);
    if (Context->Service == NULL)
        goto fail3;

    Context->EventLog = RegisterEventSource(NULL,
                                            MONITOR_NAME);
    if (Context->EventLog == NULL)
        goto fail4;

    Context->Status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    Context->Status.dwServiceSpecificExitCode = 0;

    ReportStatus(SERVICE_START_PENDING, NO_ERROR, 3000);

    Context->StopEvent = CreateEvent(NULL,
                                     TRUE,
                                     FALSE,
                                     NULL);

    if (Context->StopEvent == NULL)
        goto fail5;

    Context->RequestEvent = CreateEvent(NULL,
                                        TRUE,
                                        FALSE,
                                        NULL);
    if (Context->RequestEvent == NULL)
        goto fail6;

    Context->ResponseEvent = CreateEvent(NULL,
                                         FALSE,
                                         FALSE,
                                         NULL);
    if (Context->ResponseEvent == NULL)
        goto fail7;
    Context->Response = 0;

    Success = GetRequestKeyName(&RequestKeyName);
    if (!Success)
        goto fail8;

    Error = RegCreateKeyEx(HKEY_LOCAL_MACHINE,
                           RequestKeyName,
                           0,
                           NULL,
                           REG_OPTION_NON_VOLATILE,
                           KEY_ALL_ACCESS,
                           NULL,
                           &Context->RequestKey,
                           NULL);
    if (Error != ERROR_SUCCESS)
        goto fail9;

    Success = GetDialogParameters();
    if (!Success)
        goto fail10;

    Context->Timer = CreateWaitableTimer(NULL, FALSE, NULL);
    if (Context->Timer == NULL)
        goto fail11;

    DueTime.QuadPart = -10000LL * REBOOT_RETRY_DELAY;

    Success = SetWaitableTimer(Context->Timer,
                               &DueTime,
                               REBOOT_RETRY_DELAY,
                               NULL,
                               NULL,
                               FALSE);
    if (!Success)
        goto fail12;

    SetEvent(Context->RequestEvent);

    ReportStatus(SERVICE_RUNNING, NO_ERROR, 0);

    for (;;) {
        HANDLE  Events[4];
        DWORD   Object;

        Events[0] = Context->StopEvent;
        Events[1] = Context->RequestEvent;
        Events[2] = Context->ResponseEvent;
        Events[3] = Context->Timer;

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

        case WAIT_OBJECT_0 + 2:
            if (Context->Response == IDYES || Context->Response == IDTIMEOUT)
                DoReboot(NULL, 0);
            break;

        case WAIT_OBJECT_0 + 3:
            if (Context->RebootRequestedBy)
                TryAutoReboot(Context->RebootRequestedBy);
            break;

        default:
            break;
        }
    }

done:
    free(Context->RebootRequestedBy);
    CancelWaitableTimer(Context->Timer);
    CloseHandle(Context->Timer);

    (VOID) RegDeleteTree(Context->RequestKey, NULL);

    free(Context->Question);
    free(Context->Text);
    free(Context->Title);
    CloseHandle(Context->RequestKey);
    free(RequestKeyName);
    CloseHandle(Context->ResponseEvent);
    CloseHandle(Context->RequestEvent);
    CloseHandle(Context->StopEvent);

    ReportStatus(SERVICE_STOPPED, NO_ERROR, 0);

    (VOID) DeregisterEventSource(Context->EventLog);

    CloseHandle(Context->ParametersKey);
    (VOID) RemoveStartOverride("stornvme");

    Log("<====");

    return;

fail12:
    Log("fail12");

    CloseHandle(Context->Timer);

fail11:
    Log("fail11");

fail10:
    Log("fail10");

    CloseHandle(Context->RequestKey);

fail9:
    Log("fail9");

    free(RequestKeyName);

fail8:
    Log("fail8");

    CloseHandle(Context->ResponseEvent);

fail7:
    Log("fail7");

    CloseHandle(Context->RequestEvent);

fail6:
    Log("fail6");

    CloseHandle(Context->StopEvent);

fail5:
    Log("fail5");

    ReportStatus(SERVICE_STOPPED, GetLastError(), 0);

    (VOID) DeregisterEventSource(Context->EventLog);

fail4:
    Log("fail4");

fail3:
    Log("fail3");

fail2:
    Log("fail2");

    CloseHandle(Context->ParametersKey);

fail1:
    Error = GetLastError();

    {
        PTSTR   Message;
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
        PTSTR   Message;
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
        PTSTR   Message;
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
        PTSTR   Message;
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
