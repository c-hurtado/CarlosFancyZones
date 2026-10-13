#pragma once

#include <optional>
#include <vector>

#include <FancyZonesLib/DockTree.h>
#include <FancyZonesLib/Layout.h>
#include <FancyZonesLib/LayoutAssignedWindows.h>

class ZonesOverlay;

class WorkArea
{
    WorkArea(HINSTANCE hinstance, const FancyZonesDataTypes::WorkAreaId& uniqueId, const FancyZonesUtils::Rect& workAreaRect);

public:
    ~WorkArea();

    static std::unique_ptr<WorkArea> Create(HINSTANCE hinstance, const FancyZonesDataTypes::WorkAreaId& uniqueId, const FancyZonesDataTypes::WorkAreaId& parentUniqueId, const FancyZonesUtils::Rect& workAreaRect)
    {
        auto self = std::unique_ptr<WorkArea>(new WorkArea(hinstance, uniqueId, workAreaRect));
        if (!self->Init(hinstance, parentUniqueId))
        {
            return nullptr;
        }

        return self;
    }

    inline bool Init([[maybe_unused]] HINSTANCE hinstance, const FancyZonesDataTypes::WorkAreaId& parentUniqueId)
    {
#ifndef UNIT_TESTS
        if (!InitWindow(hinstance))
        {
            return false;
        }
#endif
        InitLayout(parentUniqueId);
        
        return true;
    }
    
    FancyZonesDataTypes::WorkAreaId UniqueId() const noexcept { return { m_uniqueId }; }
    const std::unique_ptr<Layout>& GetLayout() const noexcept { return m_layout; }
    const LayoutAssignedWindows& GetLayoutWindows() const noexcept { return m_layoutWindows; }
    const HWND GetWorkAreaWindow() const noexcept { return m_window; }
    const GUID GetLayoutId() const noexcept;
    const FancyZonesUtils::Rect& GetWorkAreaRect() const noexcept { return m_workAreaRect; }
    
    void InitLayout();
    void InitSnappedWindows();
    void UpdateWindowPositions();

    bool Snap(HWND window, const ZoneIndexSet& zones, bool updatePosition = true);
    bool Unsnap(HWND window);

    void ShowZones(const ZoneIndexSet& highlight, HWND draggedWindow = nullptr);
    void HideZones();
    void FlashZones();
    void ShowLayoutNameLabel(const std::wstring& name);
    void ShowMonitorRotationPreview(const std::vector<RECT>& windowRects, size_t monitorNumber, std::optional<bool> reverse, bool animateRotation);
    
    void CycleWindows(HWND window, bool reverse);

    // Docking. Points are in work area window coordinates.
    Dock::Tree& DockTree() const;
    void ShowDockPreview(POINT pt, HWND draggedWindow);
    bool DockWindow(HWND window, POINT pt);
    bool UndockWindow(HWND window);
    // Docks the window against that edge of the work area
    void DockAtEdge(HWND window, Dock::Edge edge);
    // Swaps a docked window with its neighbor on that side. False when it has none.
    bool DockMove(HWND window, Dock::Edge direction);
    bool DockResized(HWND window, const RECT& fromScreenRect);
    bool DockSetHidden(HWND window, bool hidden);
    // Gives the window the empty slot kept for its app, if there is one
    bool DockClaim(HWND window);
    void DockRecall(Dock::Tree tree);
    void ApplyDockLayout();

protected:
    static LRESULT CALLBACK s_WndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept;

private:
    bool InitWindow(HINSTANCE hinstance);
    void InitLayout(const FancyZonesDataTypes::WorkAreaId& parentUniqueId);
    
    void CalculateZoneSet();
    void SetWorkAreaWindowAsTopmost(HWND draggedWindow) noexcept;

    LRESULT WndProc(UINT message, WPARAM wparam, LPARAM lparam) noexcept;
    
    const FancyZonesUtils::Rect m_workAreaRect{};
    const FancyZonesDataTypes::WorkAreaId m_uniqueId;
    HWND m_window{}; // Hidden tool window used to represent current monitor desktop work area.
    std::unique_ptr<Layout> m_layout;
    LayoutAssignedWindows m_layoutWindows{};
    std::unique_ptr<ZonesOverlay> m_zonesOverlay;
    std::optional<RECT> m_dockPreview; // Dock target currently drawn on the overlay
};
