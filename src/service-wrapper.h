#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>

// -----------------------------------------------------------------------
//  Constants
// -----------------------------------------------------------------------
#define SW_VERSION            L"1.0.0"
#define SW_LOG_SOURCE         L"ServiceWrapper"
#define SW_CONFIG_FILE        L"service-wrapper.ini"
#define SW_MAX_LOG_MSG        4096

#define INI_SECTION           L"ServiceWrapper"
#define INI_KEY_EXE           L"Executable"
#define INI_KEY_ARGS          L"Arguments"
#define INI_KEY_WORKDIR       L"WorkingDirectory"
#define INI_KEY_RESTART       L"RestartOnCrash"
#define INI_KEY_RESTART_DELAY L"RestartDelaySeconds"
#define INI_KEY_SVC_NAME      L"ServiceName"
#define INI_KEY_SVC_DISP      L"DisplayName"
#define INI_KEY_SVC_DESC      L"Description"
#define INI_KEY_LOG_FILE      L"LogFile"

// -----------------------------------------------------------------------
//  Config structure — all members use m_ prefix
// -----------------------------------------------------------------------
struct ServiceConfig {
    std::wstring m_service_name;
    std::wstring m_display_name;
    std::wstring m_description;
    std::wstring m_executable;
    std::wstring m_arguments;
    std::wstring m_working_directory;
    std::wstring m_log_file;
    bool         m_restart_on_crash = false;
    DWORD        m_restart_delay_ms = 5000;
};

// -----------------------------------------------------------------------
//  Core service functions  (service-wrapper.cpp)
// -----------------------------------------------------------------------
bool LoadConfig(const std::wstring& ini_path, ServiceConfig& config);
void LogMessage(WORD type, const wchar_t* fmt, ...);

void WINAPI ServiceMain(DWORD argc, LPWSTR* argv);
DWORD WINAPI ServiceCtrlHandler(DWORD ctrl, DWORD evt_type, LPVOID evt_data, LPVOID ctx);

bool StartChildProcess();
void StopChildProcess(DWORD timeout_ms);
void ReportServiceStatus(DWORD state, DWORD exit_code, DWORD wait_hint);

// -----------------------------------------------------------------------
//  Install / management helpers  (install.cpp)
// -----------------------------------------------------------------------
int  CmdInstall(const ServiceConfig& config, const std::wstring& ini_path);
int  CmdUninstall(const std::wstring& service_name);
int  CmdStart(const std::wstring& service_name);
int  CmdStop(const std::wstring& service_name);
void PrintUsage(const wchar_t* exe_name);
