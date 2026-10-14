#pragma once

#include <algorithm>
#include <map>
#include <optional>
#include <string>

#include <FancyZonesLib/DockTree.h>
#include <FancyZonesLib/FancyZonesDataTypes.h>
#include <FancyZonesLib/ModuleConstants.h>

#include <common/SettingsAPI/settings_helpers.h>

namespace NonLocalizable
{
    namespace DockLayoutsIds
    {
        const static wchar_t* WorkAreasID = L"work-areas";
        const static wchar_t* SlotsID = L"slots";
        const static wchar_t* NamedID = L"named";
        const static wchar_t* AppID = L"app";
        const static wchar_t* LaunchID = L"launch";
        const static wchar_t* WindowID = L"window";
        const static wchar_t* ProcessID = L"process";
        const static wchar_t* SideBySideID = L"side-by-side";
        const static wchar_t* RatioID = L"ratio";
        const static wchar_t* FirstID = L"first";
        const static wchar_t* SecondID = L"second";
    }
}

namespace DockCommands
{
    // FanciestZones.exe save|load|delete <name> sends "<verb>\n<name>" to the running instance with WM_COPYDATA
    constexpr ULONG_PTR CopyDataId = 0x465A4443;
    // Class of the main window of the running instance
    constexpr wchar_t WindowClassName[] = L"SuperFancyZones";

    // A layout name is also the name of a Start menu shortcut file, and shell arguments expand %
    inline bool IsValidName(const std::wstring& name) noexcept
    {
        static constexpr const wchar_t* reserved[] = { L"CON", L"PRN", L"AUX", L"NUL", L"COM1", L"COM2", L"COM3", L"COM4", L"COM5", L"COM6", L"COM7", L"COM8", L"COM9", L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9" };
        return !name.empty() && name.size() <= 100 && name.back() != L'.' && name.back() != L' ' &&
               std::none_of(name.begin(), name.end(), [](wchar_t c) { return c < 0x20 || std::wstring_view{ L"\\/:*?\"<>|%" }.find(c) != std::wstring_view::npos; }) &&
               std::none_of(std::begin(reserved), std::end(reserved), [&](const wchar_t* device) { return _wcsicmp(name.c_str(), device) == 0; });
    }

    // Names are matched like the shortcut files they come with: ignoring case
    struct NameLess
    {
        bool operator()(const std::wstring& a, const std::wstring& b) const noexcept { return _wcsicmp(a.c_str(), b.c_str()) < 0; }
    };
}

// Dock layouts of every work area, and the layouts saved by hotkey or by name
class DockLayouts
{
public:
    static DockLayouts& instance();

    inline static std::wstring DockLayoutsFileName()
    {
        std::wstring saveFolderPath = PTSettingsHelper::get_module_save_folder_location(NonLocalizable::ModuleKey);
        return saveFolderPath + L"\\dock-layouts.json";
    }

    // Minimized, not visible, or on another virtual desktop: the window takes no space in its layout
    static bool IsHidden(HWND window) noexcept;
    // Drops the windows that don't exist anymore, keeping their slots for the app, and updates the hidden state of the rest
    static void Refresh(Dock::Tree& tree);

    Dock::Tree& Tree(const FancyZonesDataTypes::WorkAreaId& id);
    // Removes the window from the layouts of all work areas, the ones of other virtual desktops included
    bool RemoveWindow(HWND window);
    // Drops the layouts of virtual desktops that don't exist anymore
    void SyncVirtualDesktops(const GUID& currentVirtualDesktop, const std::optional<std::vector<GUID>>& desktops);

    bool HasSlot(int key) const noexcept;
    void SaveSlot(int key, const Dock::Tree& tree);
    std::optional<Dock::Tree> Slot(int key) const;

    // A named layout holds a tree per monitor, keyed by MonitorId::toString(), whatever the virtual desktop
    bool HasNamed(const std::wstring& name) const noexcept;
    void SaveNamed(const std::wstring& name, std::map<std::wstring, Dock::Tree> trees);
    bool DeleteNamed(const std::wstring& name);
    std::map<std::wstring, Dock::Tree> Named(const std::wstring& name) const;

    void SaveData() const;

private:
    DockLayouts();
    ~DockLayouts() = default;

    std::map<std::wstring, Dock::Tree> m_trees;
    std::map<int, Dock::Tree> m_slots;
    std::map<std::wstring, std::map<std::wstring, Dock::Tree>, DockCommands::NameLess> m_named;
};
