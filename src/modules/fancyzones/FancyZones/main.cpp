#include "pch.h"

#include <common/utils/ProcessWaiter.h>
#include <common/utils/window.h>
#include <common/utils/UnhandledExceptionHandler.h>
#include <common/utils/gpo.h>

#include <FancyZonesLib/trace.h>
#include <FancyZonesLib/Generated Files/resource.h>

#include <common/utils/logger_helper.h>
#include <common/hooks/LowlevelKeyboardEvent.h>
#include <common/utils/resources.h>

#include <common/Telemetry/EtwTrace/EtwTrace.h>

#include <FancyZonesLib/FancyZones.h>
#include <FancyZonesLib/FancyZonesWinHookEventIDs.h>
#include <FancyZonesLib/ModuleConstants.h>
#include <FancyZonesLib/FancyZonesData/DockLayouts.h>

#include <KnownFolders.h>
#include <shellapi.h>
#include <ShlObj.h>
#include <wil/resource.h>
#include <common/utils/json.h>

#include <FancyZonesApp.h>

// Non-localizable
const std::wstring moduleName = L"FancyZones";
const std::wstring internalPath = L"";
const std::wstring instanceMutexName = L"Local\\PowerToys_FancyZones_InstanceMutex";

namespace
{
    constexpr wchar_t Usage[] = L"Usage: FanciestZones.exe list | save <name> | load <name> | delete <name> | restart\n"
                                L"A name has at most 100 characters and none of \\ / : * ? \" < > | %\n";

    // A GUI app has no console of its own: write to the redirected output, or to the console of the shell that started it
    void Print(const std::wstring& text)
    {
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        wil::unique_hfile console;
        if ((!out || out == INVALID_HANDLE_VALUE) && AttachConsole(ATTACH_PARENT_PROCESS))
        {
            console.reset(CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
            out = console.get();
        }

        DWORD written = 0;
        if (!out || out == INVALID_HANDLE_VALUE)
        {
            return;
        }

        if (GetFileType(out) == FILE_TYPE_CHAR)
        {
            WriteConsoleW(out, text.c_str(), static_cast<DWORD>(text.size()), &written, nullptr);
        }
        else
        {
            const auto utf8 = winrt::to_string(text);
            WriteFile(out, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
        }
    }

    // Names of the saved layouts, one per line. Read from the file: no running instance is needed.
    std::wstring LayoutNames()
    {
        std::wstring names;
        const auto data = json::from_file(DockLayouts::DockLayoutsFileName());
        if (data && json::has(data.value(), NonLocalizable::DockLayoutsIds::NamedID))
        {
            for (const auto& entry : data->GetNamedObject(NonLocalizable::DockLayoutsIds::NamedID))
            {
                names += std::wstring{ entry.Key() } + L"\n";
            }
        }

        return names;
    }

    // FanciestZones.exe list | save|load|delete <name>: "<verb>\n<name>" for the running instance, see FancyZones::DockCommand
    std::optional<std::pair<std::wstring, std::wstring>> DockCommandFromCommandLine()
    {
        int count = 0;
        wil::unique_hlocal_ptr<LPWSTR> args(CommandLineToArgvW(GetCommandLineW(), &count));
        if (!args || count < 2)
        {
            return std::nullopt;
        }

        const std::wstring verb = args.get()[1];
        if (verb == L"list" || verb == L"restart")
        {
            // These take no name: anything more gets the usage
            return std::pair{ count == 2 ? verb : L"help", std::wstring{} };
        }

        if (verb != L"save" && verb != L"load" && verb != L"delete" && verb != L"list" && verb != L"restart" && verb != L"help" && verb != L"/?")
        {
            return std::nullopt;
        }

        // A command that can't be run fails here, instead of starting FancyZones
        const std::wstring name = count == 3 ? args.get()[2] : L"";
        return std::pair{ verb, DockCommands::IsValidName(name) ? name : std::wstring{} };
    }

    // Ends the running instance. Layouts are saved on every change, nothing is lost.
    bool EndInstance(HWND window)
    {
        DWORD processId = 0;
        GetWindowThreadProcessId(window, &processId);
        wil::unique_handle process(OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, processId));
        return process && TerminateProcess(process.get(), 0) && WaitForSingleObject(process.get(), 5000) == WAIT_OBJECT_0;
    }

    // Starts an instance as a process of its own, and returns its main window once it is up
    HWND StartInstance()
    {
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
        STARTUPINFOW startup{ .cb = sizeof(STARTUPINFOW) };
        PROCESS_INFORMATION info{};
        if (!CreateProcessW(exe, nullptr, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &info))
        {
            return nullptr;
        }

        CloseHandle(info.hThread);
        CloseHandle(info.hProcess);
        for (int i = 0; i < 100; ++i)
        {
            if (const HWND window = FindWindowW(DockCommands::WindowClassName, nullptr))
            {
                return window;
            }

            Sleep(100);
        }

        return nullptr;
    }

    bool SendDockCommand(HWND window, const std::wstring& verb, const std::wstring& name)
    {
        std::wstring text = verb + L"\n" + name;
        COPYDATASTRUCT data{ .dwData = DockCommands::CopyDataId, .cbData = static_cast<DWORD>(text.size() * sizeof(wchar_t)), .lpData = text.data() };
        DWORD_PTR result = FALSE;
        return SendMessageTimeoutW(window, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG, 10000, &result) && result;
    }

    // Start menu shortcut "<name>" that loads the layout, so that Command Palette and Windows search find it by name
    void UpdateLayoutShortcut(const std::wstring& verb, const std::wstring& name)
    {
        wil::unique_cotaskmem_string programs;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &programs)))
        {
            Logger::error(L"Layout shortcut: no Start menu folder");
            return;
        }

        const std::wstring folder = std::wstring{ programs.get() } + L"\\FanciestZones";
        const std::wstring path = folder + L"\\" + name + L".lnk";
        if (verb == L"delete")
        {
            DeleteFileW(path.c_str());
            return;
        }

        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
        CreateDirectoryW(folder.c_str(), nullptr);

        auto link = winrt::try_create_instance<IShellLinkW>(CLSID_ShellLink);
        if (!link)
        {
            return;
        }

        link->SetPath(exe);
        link->SetArguments((L"load \"" + name + L"\"").c_str());
        link->SetDescription((L"FanciestZones layout " + name).c_str());
        link->SetIconLocation(exe, 0);
        const auto file = link.try_as<IPersistFile>();
        if (!file || FAILED(file->Save(path.c_str(), TRUE)))
        {
            Logger::error(L"Layout shortcut {} couldn't be written", path);
        }
    }
}

int WINAPI wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ PWSTR lpCmdLine, _In_ int nCmdShow)
{
    Shared::Trace::ETWTrace trace{};
    trace.UpdateState(true);

    winrt::init_apartment();
    LoggerHelpers::init_logger(moduleName, internalPath, LogSettings::fancyZonesLoggerName);

    if (powertoys_gpo::getConfiguredFancyZonesEnabledValue() == powertoys_gpo::gpo_rule_configured_disabled)
    {
        Logger::warn(L"Tried to start with a GPO policy setting the utility to always be disabled. Please contact your systems administrator.");
        return 0;
    }

    InitUnhandledExceptionHandler();

    // A command for the running instance. A command process always exits: a shell waits for it when its output is piped.
    const auto command = DockCommandFromCommandLine();
    if (command)
    {
        const auto& [verb, name] = command.value();
        if (verb == L"list")
        {
            Print(LayoutNames());
            return 0;
        }

        if (verb != L"restart" && name.empty())
        {
            Print(Usage);
            return 1;
        }

        HWND running = FindWindowW(DockCommands::WindowClassName, nullptr);
        if (verb == L"restart" && running && !EndInstance(running))
        {
            Print(L"Couldn't end the running FanciestZones\n");
            return 1;
        }

        if (verb == L"restart" || (verb == L"load" && !running))
        {
            running = StartInstance();
            if (!running)
            {
                Print(L"FanciestZones didn't start\n");
                return 1;
            }

            if (verb == L"restart")
            {
                return 0;
            }
        }

        if (!running)
        {
            Print(L"FanciestZones isn't running\n");
            return 1;
        }

        const bool done = SendDockCommand(running, verb, name);
        if (done || verb == L"delete")
        {
            UpdateLayoutShortcut(verb, name);
        }

        if (!done)
        {
            Print(verb == L"save" ? L"Nothing docked to save\n" : L"No layout named " + name + L"\n");
        }

        return done ? 0 : 1;
    }

    auto mutex = CreateMutex(nullptr, true, instanceMutexName.c_str());
    if (mutex == nullptr)
    {
        Logger::error(L"Failed to create mutex. {}", get_last_error_or_default(GetLastError()));
    }

    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        Logger::warn(L"FancyZones instance is already running");
        return 0;
    }

    auto process = GetCurrentProcess();
    if (!SetPriorityClass(process, NORMAL_PRIORITY_CLASS))
    {
        std::wstring err = get_last_error_or_default(GetLastError());
        Logger::warn(L"Failed to set priority to FancyZones: {}", err);
    }

    // Started by the PowerToys runner with its process id
    std::wstring pid = std::wstring(lpCmdLine);
    if (!std::all_of(pid.begin(), pid.end(), iswdigit))
    {
        pid.clear();
    }
    if (!pid.empty())
    {
        auto mainThreadId = GetCurrentThreadId();
        ProcessWaiter::OnProcessTerminate(pid, [mainThreadId](int err) {
            if (err != ERROR_SUCCESS)
            {
                Logger::error(L"Failed to wait for parent process exit. {}", get_last_error_or_default(err));
            }
            else
            {
                Logger::trace(L"PowerToys runner exited.");
            }

            Logger::trace(L"Exiting FancyZones");
            PostThreadMessage(mainThreadId, WM_QUIT, 0, 0);
        });
    }

    Trace::RegisterProvider();

    FancyZonesApp app(GET_RESOURCE_STRING(IDS_FANCYZONES), NonLocalizable::ModuleKey);
    app.Run();

    run_message_loop();

    Trace::UnregisterProvider();

    trace.Flush();

    return 0;
}
