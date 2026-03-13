/*
 * service-wrapper.cpp
 *
 * Wraps any Windows executable as a Windows Service.
 * Configuration is read from service-wrapper.ini (next to the .exe).
 *
 * CLI:
 *   service-wrapper.exe --install      Register the service
 *   service-wrapper.exe --uninstall    Remove the service
 *   service-wrapper.exe --start        Start the installed service
 *   service-wrapper.exe --stop         Stop the running service
 *   service-wrapper.exe --run          Entry point called by SCM
 *   service-wrapper.exe --help         Show help
 */

#include "service-wrapper.h"
#include <strsafe.h>
#include <shlwapi.h>
#include <cstdio>
#include <cstdarg>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "advapi32.lib")

// -----------------------------------------------------------------------
//  Module-level globals  (snake_case, no m_ — not struct/class members)
// -----------------------------------------------------------------------
static ServiceConfig          g_config;
static SERVICE_STATUS         g_svc_status   = {};
static SERVICE_STATUS_HANDLE  g_svc_handle   = nullptr;
static HANDLE                 g_stop_event   = nullptr;
static HANDLE                 g_child_proc   = nullptr;
static HANDLE                 g_child_thread = nullptr;
static HANDLE                 g_log_file     = INVALID_HANDLE_VALUE;
static WCHAR                  g_ini_path[MAX_PATH] = {};

// -----------------------------------------------------------------------
//  Logging
// -----------------------------------------------------------------------
void LogMessage(WORD type, const wchar_t* fmt, ...)
{
    wchar_t msg[SW_MAX_LOG_MSG];
    va_list va;
    va_start(va, fmt);
    StringCchVPrintfW(msg, _countof(msg), fmt, va);
    va_end(va);

    HANDLE h_src = RegisterEventSourceW(nullptr, SW_LOG_SOURCE);
    if (h_src) {
        const wchar_t* strings[1] = { msg };
        ReportEventW(h_src, type, 0, 0x01, nullptr, 1, 0, strings, nullptr);
        DeregisterEventSource(h_src);
    }

    if (g_log_file != INVALID_HANDLE_VALUE) {
        SYSTEMTIME st;
        GetLocalTime(&st);

        wchar_t line[SW_MAX_LOG_MSG + 64];
        StringCchPrintfW(line, _countof(line),
            L"[%04d-%02d-%02d %02d:%02d:%02d] %s\r\n",
            st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond,
            msg);

        char utf8[SW_MAX_LOG_MSG * 3];
        int byte_count = WideCharToMultiByte(
            CP_UTF8, 0, line, -1, utf8, sizeof(utf8), nullptr, nullptr);

        if (byte_count > 1) {
            DWORD written;
            WriteFile(g_log_file, utf8, byte_count - 1, &written, nullptr);
            FlushFileBuffers(g_log_file);
        }
    }
}

// -----------------------------------------------------------------------
//  INI helper
// -----------------------------------------------------------------------
static std::wstring IniReadKey(const wchar_t* key, const wchar_t* default_val = L"")
{
    wchar_t buf[1024] = {};
    GetPrivateProfileStringW(INI_SECTION, key, default_val, buf, _countof(buf), g_ini_path);
    return buf;
}

bool LoadConfig(const std::wstring& ini_path, ServiceConfig& config)
{
    StringCchCopyW(g_ini_path, _countof(g_ini_path), ini_path.c_str());

    config.m_executable       = IniReadKey(INI_KEY_EXE);
    config.m_arguments        = IniReadKey(INI_KEY_ARGS);
    config.m_working_directory= IniReadKey(INI_KEY_WORKDIR);
    config.m_service_name     = IniReadKey(INI_KEY_SVC_NAME, L"WrappedService");
    config.m_display_name     = IniReadKey(INI_KEY_SVC_DISP, L"Wrapped Service");
    config.m_description      = IniReadKey(INI_KEY_SVC_DESC, L"A process wrapped as a Windows service.");
    config.m_log_file         = IniReadKey(INI_KEY_LOG_FILE);

    std::wstring restart_val  = IniReadKey(INI_KEY_RESTART, L"0");
    config.m_restart_on_crash = (restart_val == L"1" || restart_val == L"true" || restart_val == L"yes");

    std::wstring delay_val    = IniReadKey(INI_KEY_RESTART_DELAY, L"5");
    config.m_restart_delay_ms = static_cast<DWORD>(_wtoi(delay_val.c_str()) * 1000);
    if (config.m_restart_delay_ms == 0) {
        config.m_restart_delay_ms = 5000;
    }

    if (config.m_working_directory.empty()) {
        wchar_t dir[MAX_PATH];
        StringCchCopyW(dir, _countof(dir), g_ini_path);
        PathRemoveFileSpecW(dir);
        config.m_working_directory = dir;
    }

    return !config.m_executable.empty();
}

// -----------------------------------------------------------------------
//  Child process management
// -----------------------------------------------------------------------
bool StartChildProcess()
{
    std::wstring cmd_line = L"\"" + g_config.m_executable + L"\"";
    if (!g_config.m_arguments.empty()) {
        cmd_line += L" " + g_config.m_arguments;
    }

    STARTUPINFOW si = {};
    si.cb = sizeof(si);

    PROCESS_INFORMATION pi = {};

    const wchar_t* work_dir = g_config.m_working_directory.empty()
        ? nullptr
        : g_config.m_working_directory.c_str();

    BOOL created = CreateProcessW(
        nullptr,
        const_cast<LPWSTR>(cmd_line.c_str()),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        work_dir,
        &si,
        &pi);

    if (!created) {
        LogMessage(EVENTLOG_ERROR_TYPE,
            L"CreateProcess failed for \"%s\" (error %lu)",
            cmd_line.c_str(), GetLastError());
        return false;
    }

    g_child_proc   = pi.hProcess;
    g_child_thread = pi.hThread;

    LogMessage(EVENTLOG_INFORMATION_TYPE,
        L"Started child process PID=%lu  cmd=%s",
        pi.dwProcessId, cmd_line.c_str());

    return true;
}

void StopChildProcess(DWORD timeout_ms)
{
    if (!g_child_proc) {
        return;
    }

    GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, 0);

    if (WaitForSingleObject(g_child_proc, timeout_ms) != WAIT_OBJECT_0) {
        LogMessage(EVENTLOG_WARNING_TYPE,
            L"Child did not exit gracefully – terminating.");
        TerminateProcess(g_child_proc, 0);
        WaitForSingleObject(g_child_proc, 3000);
    }

    CloseHandle(g_child_proc);
    g_child_proc = nullptr;

    CloseHandle(g_child_thread);
    g_child_thread = nullptr;

    LogMessage(EVENTLOG_INFORMATION_TYPE, L"Child process stopped.");
}

// -----------------------------------------------------------------------
//  SCM status helper
// -----------------------------------------------------------------------
void ReportServiceStatus(DWORD state, DWORD exit_code, DWORD wait_hint)
{
    static DWORD check_point = 1;

    g_svc_status.dwServiceType             = SERVICE_WIN32_OWN_PROCESS;
    g_svc_status.dwCurrentState            = state;
    g_svc_status.dwWin32ExitCode           = exit_code;
    g_svc_status.dwServiceSpecificExitCode = 0;
    g_svc_status.dwWaitHint                = wait_hint;

    bool is_steady = (state == SERVICE_RUNNING || state == SERVICE_STOPPED);
    g_svc_status.dwCheckPoint = is_steady ? 0 : check_point++;

    g_svc_status.dwControlsAccepted = (state == SERVICE_RUNNING)
        ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_PAUSE_CONTINUE)
        : 0;

    SetServiceStatus(g_svc_handle, &g_svc_status);
}

// -----------------------------------------------------------------------
//  Service control handler
// -----------------------------------------------------------------------
DWORD WINAPI ServiceCtrlHandler(DWORD ctrl, DWORD /*evt_type*/, LPVOID /*evt_data*/, LPVOID /*ctx*/)
{
    if (ctrl == SERVICE_CONTROL_STOP || ctrl == SERVICE_CONTROL_SHUTDOWN) {
        LogMessage(EVENTLOG_INFORMATION_TYPE, L"Stop/Shutdown control received.");
        ReportServiceStatus(SERVICE_STOP_PENDING, NO_ERROR, 15000);
        SetEvent(g_stop_event);
    } else if (ctrl == SERVICE_CONTROL_PAUSE) {
        LogMessage(EVENTLOG_INFORMATION_TYPE, L"Pause control received.");
        ReportServiceStatus(SERVICE_PAUSED, NO_ERROR, 0);
    } else if (ctrl == SERVICE_CONTROL_CONTINUE) {
        LogMessage(EVENTLOG_INFORMATION_TYPE, L"Continue control received.");
        ReportServiceStatus(SERVICE_RUNNING, NO_ERROR, 0);
    } else if (ctrl == SERVICE_CONTROL_INTERROGATE) {
        SetServiceStatus(g_svc_handle, &g_svc_status);
    }

    return NO_ERROR;
}

// -----------------------------------------------------------------------
//  Monitor thread — watches child process, restarts if configured
// -----------------------------------------------------------------------
static DWORD WINAPI MonitorThread(LPVOID /*param*/)
{
    while (true) {
        if (!g_child_proc) {
            SetEvent(g_stop_event);
            break;
        }

        HANDLE wait_handles[2] = { g_stop_event, g_child_proc };
        DWORD wait_result = WaitForMultipleObjects(2, wait_handles, FALSE, INFINITE);

        if (wait_result == WAIT_OBJECT_0) {
            break;
        }

        if (wait_result == WAIT_OBJECT_0 + 1) {
            DWORD child_exit_code = 0;
            GetExitCodeProcess(g_child_proc, &child_exit_code);

            CloseHandle(g_child_proc);
            g_child_proc = nullptr;

            CloseHandle(g_child_thread);
            g_child_thread = nullptr;

            LogMessage(EVENTLOG_WARNING_TYPE,
                L"Child process exited with code %lu.", child_exit_code);

            bool should_restart = g_config.m_restart_on_crash && (child_exit_code != 0);
            if (!should_restart) {
                SetEvent(g_stop_event);
                break;
            }

            LogMessage(EVENTLOG_INFORMATION_TYPE,
                L"Restarting child process in %lu ms...",
                g_config.m_restart_delay_ms);

            DWORD delay_result = WaitForSingleObject(g_stop_event, g_config.m_restart_delay_ms);
            if (delay_result == WAIT_OBJECT_0) {
                break;
            }

            if (!StartChildProcess()) {
                SetEvent(g_stop_event);
                break;
            }
        }
    }

    return 0;
}

// -----------------------------------------------------------------------
//  ServiceMain — entry point invoked by the SCM
// -----------------------------------------------------------------------
void WINAPI ServiceMain(DWORD /*argc*/, LPWSTR* /*argv*/)
{
    if (!g_config.m_log_file.empty()) {
        g_log_file = CreateFileW(
            g_config.m_log_file.c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);

        if (g_log_file != INVALID_HANDLE_VALUE) {
            SetFilePointer(g_log_file, 0, nullptr, FILE_END);
        }
    }

    g_svc_handle = RegisterServiceCtrlHandlerExW(
        g_config.m_service_name.c_str(),
        ServiceCtrlHandler,
        nullptr);

    if (!g_svc_handle) {
        LogMessage(EVENTLOG_ERROR_TYPE,
            L"RegisterServiceCtrlHandlerEx failed: %lu", GetLastError());
        return;
    }

    ReportServiceStatus(SERVICE_START_PENDING, NO_ERROR, 5000);

    g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stop_event) {
        LogMessage(EVENTLOG_ERROR_TYPE,
            L"CreateEvent failed: %lu", GetLastError());
        ReportServiceStatus(SERVICE_STOPPED, GetLastError(), 0);
        return;
    }

    if (!StartChildProcess()) {
        ReportServiceStatus(SERVICE_STOPPED, ERROR_FILE_NOT_FOUND, 0);
        CloseHandle(g_stop_event);
        g_stop_event = nullptr;
        return;
    }

    ReportServiceStatus(SERVICE_RUNNING, NO_ERROR, 0);
    LogMessage(EVENTLOG_INFORMATION_TYPE,
        L"Service \"%s\" started.", g_config.m_service_name.c_str());

    HANDLE h_monitor = CreateThread(nullptr, 0, MonitorThread, nullptr, 0, nullptr);

    WaitForSingleObject(g_stop_event, INFINITE);

    ReportServiceStatus(SERVICE_STOP_PENDING, NO_ERROR, 20000);

    StopChildProcess(10000);

    if (h_monitor) {
        WaitForSingleObject(h_monitor, 5000);
        CloseHandle(h_monitor);
    }

    CloseHandle(g_stop_event);
    g_stop_event = nullptr;

    if (g_log_file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_log_file);
        g_log_file = INVALID_HANDLE_VALUE;
    }

    LogMessage(EVENTLOG_INFORMATION_TYPE,
        L"Service \"%s\" stopped.", g_config.m_service_name.c_str());

    ReportServiceStatus(SERVICE_STOPPED, NO_ERROR, 0);
}

// -----------------------------------------------------------------------
//  wmain
// -----------------------------------------------------------------------
int wmain(int argc, wchar_t* argv[])
{
    wchar_t exe_dir[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe_dir, _countof(exe_dir));
    PathRemoveFileSpecW(exe_dir);

    std::wstring ini_path = std::wstring(exe_dir) + L"\\" + SW_CONFIG_FILE;

    LoadConfig(ini_path, g_config);

    if (argc < 2) {
        SERVICE_TABLE_ENTRYW dispatch_table[] = {
            { const_cast<LPWSTR>(g_config.m_service_name.c_str()), ServiceMain },
            { nullptr, nullptr }
        };

        if (!StartServiceCtrlDispatcherW(dispatch_table)) {
            DWORD err = GetLastError();
            if (err == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
                fwprintf(stderr,
                    L"ServiceWrapper v%s\nRun with --help for usage.\n", SW_VERSION);
                return 1;
            }
        }

        return 0;
    }

    std::wstring cmd = argv[1];

    if (cmd == L"--run" || cmd == L"-run" || cmd == L"/run") {
        SERVICE_TABLE_ENTRYW dispatch_table[] = {
            { const_cast<LPWSTR>(g_config.m_service_name.c_str()), ServiceMain },
            { nullptr, nullptr }
        };
        StartServiceCtrlDispatcherW(dispatch_table);
        return 0;
    }

    if (cmd == L"--install" || cmd == L"-install" || cmd == L"/install") {
        return CmdInstall(g_config, ini_path);
    }

    if (cmd == L"--uninstall" || cmd == L"-uninstall" || cmd == L"/uninstall") {
        return CmdUninstall(g_config.m_service_name);
    }

    if (cmd == L"--start" || cmd == L"-start" || cmd == L"/start") {
        return CmdStart(g_config.m_service_name);
    }

    if (cmd == L"--stop" || cmd == L"-stop" || cmd == L"/stop") {
        return CmdStop(g_config.m_service_name);
    }

    PrintUsage(argv[0]);
    return 0;
}
