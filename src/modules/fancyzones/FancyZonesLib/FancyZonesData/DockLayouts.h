#pragma once

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
        const static wchar_t* AppID = L"app";
        const static wchar_t* WindowID = L"window";
        const static wchar_t* ProcessID = L"process";
        const static wchar_t* SideBySideID = L"side-by-side";
        const static wchar_t* RatioID = L"ratio";
        const static wchar_t* FirstID = L"first";
        const static wchar_t* SecondID = L"second";
    }
}

// Dock layouts of every work area, and the layouts saved by hotkey
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

    void SaveData() const;

private:
    DockLayouts();
    ~DockLayouts() = default;

    std::map<std::wstring, Dock::Tree> m_trees;
    std::map<int, Dock::Tree> m_slots;
};
