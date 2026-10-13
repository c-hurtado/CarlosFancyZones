#include "pch.h"
#include "FancyZones.h"

#include <common/interop/shared_constants.h>
#include <common/logger/logger.h>
#include <common/logger/call_tracer.h>
#include <common/utils/EventWaiter.h>
#include <common/utils/winapi_error.h>
#include <common/SettingsAPI/FileWatcher.h>
#include <common/notifications/NotificationUtil.h>

#include <FancyZonesLib/DraggingState.h>
#include <FancyZonesLib/EditorParameters.h>
#include <FancyZonesLib/FancyZonesData.h>
#include <FancyZonesLib/FancyZonesData/AppliedLayouts.h>
#include <FancyZonesLib/FancyZonesData/AppZoneHistory.h>
#include <FancyZonesLib/FancyZonesData/CustomLayouts.h>
#include <FancyZonesLib/FancyZonesData/DefaultLayouts.h>
#include <FancyZonesLib/FancyZonesData/DockLayouts.h>
#include <FancyZonesLib/FancyZonesData/LastUsedVirtualDesktop.h>
#include <FancyZonesLib/FancyZonesData/LayoutHotkeys.h>
#include <FancyZonesLib/FancyZonesData/LayoutTemplates.h>
#include <FancyZonesLib/FancyZonesWindowProcessing.h>
#include <FancyZonesLib/FancyZonesWindowProperties.h>
#include <FancyZonesLib/FancyZonesWinHookEventIDs.h>
#include <FancyZonesLib/KeyboardInput.h>
#include <FancyZonesLib/MonitorUtils.h>
#include <FancyZonesLib/on_thread_executor.h>
#include <FancyZonesLib/Settings.h>
#include <FancyZonesLib/SettingsObserver.h>
#include <FancyZonesLib/trace.h>
#include <FancyZonesLib/util.h>
#include <FancyZonesLib/VirtualDesktop.h>
#include <FancyZonesLib/WindowMouseSnap.h>
#include <FancyZonesLib/WindowUtils.h>
#include <FancyZonesLib/WorkArea.h>
#include <FancyZonesLib/WorkAreaConfiguration.h>

enum class DisplayChangeType
{
    WorkArea,
    DisplayChange,
    VirtualDesktop,
    Initialization
};

constexpr const wchar_t* DisplayChangeTypeName (const DisplayChangeType type){
    switch (type)
    {
    case DisplayChangeType::WorkArea:
        return L"WorkArea";
    case DisplayChangeType::DisplayChange:
        return L"DisplayChange";
    case DisplayChangeType::VirtualDesktop:
        return L"VirtualDesktop";
    case DisplayChangeType::Initialization:
        return L"Initialization";
    default:
        return L"";
    }
}

// Non-localizable strings
namespace NonLocalizable
{
    const wchar_t ToolWindowClassName[] = L"SuperFancyZones";
    const wchar_t FZEditorExecutablePath[] = L"PowerToys.FancyZonesEditor.exe";
}

namespace
{
    // Layouts the mouse wheel cycles through while dragging: the ones with quick-switch
    // hotkeys assigned (in hotkey order); when fewer than two of those exist, all custom
    // layouts in the order they appear in the editor.
    std::vector<GUID> WheelCycleLayoutIds() noexcept
    {
        auto ids = LayoutHotkeys::instance().GetLayoutIds();
        std::erase_if(ids, [](const GUID& id) { return !CustomLayouts::instance().GetCustomLayoutData(id).has_value(); });

        if (ids.size() < 2)
        {
            ids = CustomLayouts::instance().GetLayoutIdsInOrder();
        }

        return ids;
    }

    constexpr UINT_PTR MonitorRotationCommitTimerId = 0x4D525443;
    constexpr UINT MonitorRotationCommitDelayMillis = 760;

    struct WindowRotationSnapshot
    {
        HWND window{};
        HMONITOR sourceMonitor{};
        RECT sourceRect{};
    };

    std::optional<size_t> FindMonitorIndex(HMONITOR monitor, const std::vector<std::pair<HMONITOR, RECT>>& monitors) noexcept
    {
        const auto iter = std::find_if(monitors.begin(), monitors.end(), [monitor](const auto& entry) {
            return entry.first == monitor;
        });

        if (iter == monitors.end())
        {
            return std::nullopt;
        }

        return static_cast<size_t>(std::distance(monitors.begin(), iter));
    }

    std::vector<WindowRotationSnapshot> CollectWindowRotationSnapshots()
    {
        std::vector<WindowRotationSnapshot> windows;
        EnumWindows(
            [](HWND window, LPARAM param) -> BOOL {
                if (!FancyZonesWindowProcessing::IsProcessableManually(window))
                {
                    return TRUE;
                }

                RECT rect{};
                if (!GetWindowRect(window, &rect) || rect.left == rect.right || rect.top == rect.bottom)
                {
                    return TRUE;
                }

                auto sourceMonitor = MonitorFromWindow(window, MONITOR_DEFAULTTONULL);
                if (!sourceMonitor)
                {
                    return TRUE;
                }

                auto& snapshots = *reinterpret_cast<std::vector<WindowRotationSnapshot>*>(param);
                snapshots.push_back({ .window = window, .sourceMonitor = sourceMonitor, .sourceRect = rect });
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&windows));

        return windows;
    }

    constexpr bool IsRotationPreviewReleaseKey(DWORD vkCode) noexcept
    {
        return vkCode == VK_LWIN || vkCode == VK_RWIN || vkCode == VK_CONTROL || vkCode == VK_LCONTROL || vkCode == VK_RCONTROL || vkCode == VK_MENU || vkCode == VK_LMENU || vkCode == VK_RMENU || vkCode == VK_SHIFT || vkCode == VK_LSHIFT || vkCode == VK_RSHIFT;
    }

    constexpr bool IsWinKey(DWORD vkCode) noexcept
    {
        return vkCode == VK_LWIN || vkCode == VK_RWIN;
    }

    constexpr bool IsCtrlKey(DWORD vkCode) noexcept
    {
        return vkCode == VK_CONTROL || vkCode == VK_LCONTROL || vkCode == VK_RCONTROL;
    }

    constexpr bool IsAltKey(DWORD vkCode) noexcept
    {
        return vkCode == VK_MENU || vkCode == VK_LMENU || vkCode == VK_RMENU;
    }

    constexpr bool IsShiftKey(DWORD vkCode) noexcept
    {
        return vkCode == VK_SHIFT || vkCode == VK_LSHIFT || vkCode == VK_RSHIFT;
    }

    constexpr int DigitFromVkCode(DWORD vkCode) noexcept
    {
        if ('0' <= vkCode && vkCode <= '9')
        {
            return vkCode - '0';
        }

        if (VK_NUMPAD0 <= vkCode && vkCode <= VK_NUMPAD9)
        {
            return vkCode - VK_NUMPAD0;
        }

        return -1;
    }

    size_t GetMonitorDisplayNumber(HMONITOR monitor, size_t fallback) noexcept
    {
        MONITORINFOEX monitorInfo{};
        monitorInfo.cbSize = sizeof(MONITORINFOEX);
        if (!GetMonitorInfoW(monitor, &monitorInfo))
        {
            return fallback;
        }

        std::wstring deviceName = monitorInfo.szDevice;
        const auto digitStart = deviceName.find_last_not_of(L"0123456789");
        if (digitStart == std::wstring::npos || digitStart + 1 >= deviceName.size())
        {
            return fallback;
        }

        try
        {
            return static_cast<size_t>(std::stoul(deviceName.substr(digitStart + 1)));
        }
        catch (...)
        {
            return fallback;
        }
    }
}

namespace MonitorRotation
{
    LONG ScaleCoordinate(LONG value, LONG sourceStart, LONG sourceSize, LONG targetStart, LONG targetSize) noexcept
    {
        if (sourceSize == 0)
        {
            return targetStart;
        }

        return targetStart + MulDiv(value - sourceStart, targetSize, sourceSize);
    }

    RECT MapRectBetweenMonitorWorkAreas(const RECT& windowRect, const RECT& sourceWorkArea, const RECT& targetWorkArea) noexcept
    {
        const LONG sourceWidth = sourceWorkArea.right - sourceWorkArea.left;
        const LONG sourceHeight = sourceWorkArea.bottom - sourceWorkArea.top;
        const LONG targetWidth = targetWorkArea.right - targetWorkArea.left;
        const LONG targetHeight = targetWorkArea.bottom - targetWorkArea.top;

        return RECT{
            .left = ScaleCoordinate(windowRect.left, sourceWorkArea.left, sourceWidth, targetWorkArea.left, targetWidth),
            .top = ScaleCoordinate(windowRect.top, sourceWorkArea.top, sourceHeight, targetWorkArea.top, targetHeight),
            .right = ScaleCoordinate(windowRect.right, sourceWorkArea.left, sourceWidth, targetWorkArea.left, targetWidth),
            .bottom = ScaleCoordinate(windowRect.bottom, sourceWorkArea.top, sourceHeight, targetWorkArea.top, targetHeight),
        };
    }

    size_t GetRotatedMonitorIndex(size_t sourceIndex, size_t monitorCount, bool reverse) noexcept
    {
        if (reverse)
        {
            return sourceIndex == 0 ? monitorCount - 1 : sourceIndex - 1;
        }

        return (sourceIndex + 1) % monitorCount;
    }
}

struct FancyZones : public winrt::implements<FancyZones, IFancyZones, IFancyZonesCallback>, public SettingsObserver
{
public:
    FancyZones(HINSTANCE hinstance, std::function<void()> disableModuleCallbackFunction) noexcept :
        SettingsObserver({ SettingId::EditorHotkey, SettingId::WindowSwitching, SettingId::PrevTabHotkey, SettingId::NextTabHotkey, SettingId::SpanZonesAcrossMonitors, SettingId::MonitorRotation, SettingId::MonitorRotationHotkey }),
        m_hinstance(hinstance),
        m_draggingState([this]() {
            PostMessageW(m_window, WM_PRIV_LOCATIONCHANGE, NULL, NULL);
        },
        [this](bool up) {
            // Swallow the wheel event only when a switch is actually going to happen, otherwise
            // the window under the cursor would become unscrollable for nothing
            const auto workArea = m_workAreaConfiguration.GetWorkAreaFromCursor();
            if (!workArea || !NextWheelLayout(*workArea, up).has_value())
            {
                return false;
            }

            PostMessageW(m_window, WM_PRIV_WHEEL_LAYOUT_SWITCH, up ? 1 : 0, NULL);
            return true;
        })
    {
        if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL))
        {
            Logger::warn("Failed to set main thread priority");
        }

        this->disableModuleCallback = std::move(disableModuleCallbackFunction);

        FancyZonesSettings::instance().LoadSettings();

        FancyZonesDataInstance().ReplaceZoneSettingsFileFromOlderVersions();
        LayoutTemplates::instance().LoadData();
        CustomLayouts::instance().LoadData();
        LayoutHotkeys::instance().LoadData();
        AppliedLayouts::instance().LoadData();
        AppZoneHistory::instance().LoadData();
        DefaultLayouts::instance().LoadData();
        LastUsedVirtualDesktop::instance().LoadData();
    }

    // IFancyZones
    IFACEMETHODIMP_(void)
    Run() noexcept;
    IFACEMETHODIMP_(void)
    Destroy() noexcept;

    IFACEMETHODIMP_(void)
    HandleWinHookEvent(const WinHookEvent* data) noexcept
    {
        const auto wparam = reinterpret_cast<WPARAM>(data->hwnd);
        const LONG lparam = 0;
        switch (data->event)
        {
        case EVENT_SYSTEM_MOVESIZESTART:
            PostMessageW(m_window, WM_PRIV_MOVESIZESTART, wparam, lparam);
            break;
        case EVENT_SYSTEM_MOVESIZEEND:
            PostMessageW(m_window, WM_PRIV_MOVESIZEEND, wparam, lparam);
            break;
        case EVENT_OBJECT_LOCATIONCHANGE:
            PostMessageW(m_window, WM_PRIV_LOCATIONCHANGE, wparam, lparam);
            break;
        case EVENT_OBJECT_NAMECHANGE:
            PostMessageW(m_window, WM_PRIV_NAMECHANGE, wparam, lparam);
            break;

        case EVENT_OBJECT_UNCLOAKED:
        case EVENT_OBJECT_SHOW:
        case EVENT_OBJECT_CREATE:
            if (data->idObject == OBJID_WINDOW)
            {
                PostMessageW(m_window, WM_PRIV_WINDOWCREATED, wparam, lparam);
            }
            break;

        case EVENT_OBJECT_DESTROY:
            if (data->idObject == OBJID_WINDOW)
            {
                PostMessageW(m_window, WM_PRIV_WINDOWDESTROYED, wparam, lparam);
            }
            break;

        case EVENT_SYSTEM_MINIMIZESTART:
            PostMessageW(m_window, WM_PRIV_DOCK_VISIBILITY, wparam, TRUE);
            break;
        case EVENT_SYSTEM_MINIMIZEEND:
            PostMessageW(m_window, WM_PRIV_DOCK_VISIBILITY, wparam, FALSE);
            break;
        case EVENT_OBJECT_HIDE:
        case EVENT_OBJECT_CLOAKED:
            if (data->idObject == OBJID_WINDOW)
            {
                PostMessageW(m_window, WM_PRIV_DOCK_VISIBILITY, wparam, FALSE);
            }
            break;
        }
    }

    IFACEMETHODIMP_(void)
    VirtualDesktopChanged() noexcept;
    IFACEMETHODIMP_(bool)
    OnKeyDown(PKBDLLHOOKSTRUCT info) noexcept;
    IFACEMETHODIMP_(bool)
    OnKeyUp(PKBDLLHOOKSTRUCT info) noexcept;

    void MoveSizeStart(HWND window, HMONITOR monitor);
    void MoveSizeUpdate(HMONITOR monitor, POINT const& ptScreen);
    void MoveSizeEnd();
    void AbortMoveSize();

    void WindowCreated(HWND window) noexcept;
    void ToggleEditor() noexcept;

    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM) noexcept;
    void OnKeyboardInput(WPARAM flags, HRAWINPUT hInput) noexcept;
    void OnDisplayChange(DisplayChangeType changeType) noexcept;
    bool AddWorkArea(HMONITOR monitor, const FancyZonesDataTypes::WorkAreaId& id, const FancyZonesUtils::Rect& rect) noexcept;

protected:
    static LRESULT CALLBACK s_WndProc(HWND, UINT, WPARAM, LPARAM) noexcept;

private:
    void UpdateWorkAreas(bool updateWindowPositions) noexcept;
    bool ShouldWorkAreasBeRecreated(const std::vector<FancyZonesDataTypes::MonitorId>& monitors, const GUID& virtualDesktop, const std::unordered_map<HMONITOR, std::unique_ptr<WorkArea>>& workAreas) noexcept;
    void CycleWindows(bool reverse) noexcept;
    void RotateWindowsAcrossMonitors(bool reverse) noexcept;
    void ShowMonitorRotationPreview(std::optional<bool> reverse = std::nullopt, bool animateRotation = false) noexcept;
    void HideMonitorRotationPreview() noexcept;
    void EnsureMonitorRotationContentNumbers(const std::vector<std::pair<HMONITOR, RECT>>& monitors) noexcept;
    void RotateMonitorRotationContentNumbers(bool reverse) noexcept;
    bool IsMonitorRotationActivatorKey(DWORD vkCode) const noexcept;
    bool IsMonitorRotationChordDown() const noexcept;

    void SyncVirtualDesktops() noexcept;

    void UpdateHotkey(int hotkeyId, const PowerToysSettings::HotkeyObject& hotkeyObject, bool enable) noexcept;
    
    bool MoveToAppLastZone(HWND window, HMONITOR monitor, GUID currentVirtualDesktop) noexcept;

    void RefreshLayouts() noexcept;
    void ApplyQuickLayout(int key) noexcept;
    void CycleLayoutByWheel(bool reverse) noexcept;
    std::optional<LayoutData> NextWheelLayout(const WorkArea& workArea, bool reverse) const noexcept;
    void SwitchLayout(WorkArea* workArea, const LayoutData& layout) noexcept;
    void FlashZones() noexcept;

    void DockClaim(HWND window) noexcept;
    void DockVisibility(HWND window, bool minimizing) noexcept;
    void DockSlot(int key, bool save) noexcept;
    void DockMoveByKey(DWORD vkCode) noexcept;

    HMONITOR WorkAreaKeyFromWindow(HWND window) noexcept;

    virtual void SettingsUpdate(SettingId type) override;

    const HINSTANCE m_hinstance{};

    HWND m_window{};
    std::unique_ptr<WindowMouseSnap> m_windowMouseSnapper{};
    HWND m_resizedWindow{}; // Window being resized by the user
    RECT m_resizedWindowRect{}; // Its frame when the resize started
    DWORD m_dockSlotKey{}; // Digit of the dock layout hotkey that is held down
    WorkAreaConfiguration m_workAreaConfiguration;
    DraggingState m_draggingState;
    bool m_monitorRotationPreviewActive = false;
    MonitorRotation::KeyState m_monitorRotationKeyState;
    std::optional<bool> m_pendingMonitorRotationReverse;
    std::unordered_map<HMONITOR, size_t> m_monitorRotationContentNumbers;

    wil::unique_handle m_terminateEditorEvent; // Handle of FancyZonesEditor.exe we launch and wait on

    OnThreadExecutor m_dpiUnawareThread;

    EventWaiter m_toggleEditorEventWaiter;

    std::unique_ptr<notifications::NotificationUtil> m_notificationUtil;

    // If non-recoverable error occurs, trigger disabling of entire FancyZones.
    static std::function<void()> disableModuleCallback;

    // Did we terminate the editor or was it closed cleanly?
    enum class EditorExitKind : byte
    {
        Exit,
        Terminate
    };

    // IDs used to register hot keys (keyboard shortcuts).
    enum class HotkeyId : int
    {
        Editor = 1,
        NextTab = 2,
        PrevTab = 3,
    };
};

std::function<void()> FancyZones::disableModuleCallback = {};

// IFancyZones
IFACEMETHODIMP_(void)
FancyZones::Run() noexcept
{
    WNDCLASSEXW wcex{};
    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.lpfnWndProc = s_WndProc;
    wcex.hInstance = m_hinstance;
    wcex.lpszClassName = NonLocalizable::ToolWindowClassName;
    RegisterClassExW(&wcex);

    BufferedPaintInit();

    m_window = CreateWindowExW(WS_EX_TOOLWINDOW, NonLocalizable::ToolWindowClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, m_hinstance, this);
    if (!m_window)
    {
        Logger::critical(L"Failed to create FancyZones window");
        return;
    }

    if (!KeyboardInput::Initialize(m_window))
    {
        Logger::critical(L"Failed to register raw input device");
        return;
    }

    UpdateHotkey(static_cast<int>(HotkeyId::Editor), FancyZonesSettings::settings().editorHotkey, true);
    UpdateHotkey(static_cast<int>(HotkeyId::PrevTab), FancyZonesSettings::settings().prevTabHotkey, FancyZonesSettings::settings().windowSwitching);
    UpdateHotkey(static_cast<int>(HotkeyId::NextTab), FancyZonesSettings::settings().nextTabHotkey, FancyZonesSettings::settings().windowSwitching);

    // Initialize COM. Needed for WMI monitor identifying
    HRESULT comInitHres = CoInitializeEx(0, COINIT_MULTITHREADED);
    if (FAILED(comInitHres))
    {
        Logger::error(L"Failed to initialize COM library. {}", get_last_error_or_default(comInitHres));
        return;
    }

    // Initialize security. Needed for WMI monitor identifying
    HRESULT comSecurityInitHres = CoInitializeSecurity(NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE, NULL);
    if (FAILED(comSecurityInitHres))
    {
        Logger::error(L"Failed to initialize security. {}", get_last_error_or_default(comSecurityInitHres));
        return;
    }

    m_dpiUnawareThread.submit(OnThreadExecutor::task_t{ [] {
                          SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_UNAWARE);
                          SetThreadDpiHostingBehavior(DPI_HOSTING_BEHAVIOR_MIXED);
                      } })
        .wait();

    m_toggleEditorEventWaiter.start(CommonSharedConstants::FANCY_ZONES_EDITOR_TOGGLE_EVENT, [&](DWORD err) {
        if (err == ERROR_SUCCESS)
        {
            Logger::trace(L"{} event was signaled", CommonSharedConstants::FANCY_ZONES_EDITOR_TOGGLE_EVENT);
            PostMessage(m_window, WM_HOTKEY, 1, 0);
        }
    });

    m_notificationUtil = std::make_unique<notifications::NotificationUtil>();

    SyncVirtualDesktops();

    // id format of applied-layouts and app-zone-history was changed in 0.60
    auto monitors = MonitorUtils::IdentifyMonitors();
    AppliedLayouts::instance().AdjustWorkAreaIds(monitors);
    AppZoneHistory::instance().AdjustWorkAreaIds(monitors);

    PostMessage(m_window, WM_PRIV_INIT, 0, 0);
}

// IFancyZones
IFACEMETHODIMP_(void)
FancyZones::Destroy() noexcept
{
    m_workAreaConfiguration.Clear();
    BufferedPaintUnInit();
    if (m_window)
    {
        DestroyWindow(m_window);
        m_window = nullptr;
    }

    m_notificationUtil.reset();

    CoUninitialize();
}

// IFancyZonesCallback
IFACEMETHODIMP_(void)
FancyZones::VirtualDesktopChanged() noexcept
{
    // VirtualDesktopChanged is called from a reentrant WinHookProc function, therefore we must postpone the actual logic
    // until we're in FancyZones::WndProc, which is not reentrant.
    PostMessage(m_window, WM_PRIV_VD_SWITCH, 0, 0);
}

void FancyZones::MoveSizeStart(HWND window, HMONITOR monitor)
{
    m_resizedWindow = nullptr;
    m_windowMouseSnapper = WindowMouseSnap::Create(window, m_workAreaConfiguration.GetAllWorkAreas(), m_notificationUtil.get());
    if (m_windowMouseSnapper)
    {
        if (FancyZonesSettings::settings().spanZonesAcrossMonitors)
        {
            monitor = NULL;
        }

        m_draggingState.Enable();
        m_draggingState.UpdateDraggingState();
        m_windowMouseSnapper->MoveSizeStart(monitor, m_draggingState.IsDragging());
    }
    else if (FancyZonesWindowUtils::IsCursorTypeIndicatingSizeEvent() && SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &m_resizedWindowRect, sizeof(m_resizedWindowRect))))
    {
        m_resizedWindow = window;
    }
}

void FancyZones::MoveSizeUpdate(HMONITOR monitor, POINT const& ptScreen)
{
    if (m_windowMouseSnapper)
    {
        if (FancyZonesSettings::settings().spanZonesAcrossMonitors)
        {
            monitor = NULL;
        }

        m_draggingState.UpdateDraggingState();
        m_windowMouseSnapper->MoveSizeUpdate(monitor, ptScreen, m_draggingState.IsDragging(), m_draggingState.IsSelectManyZonesState());
    }
}

void FancyZones::MoveSizeEnd()
{
    if (m_windowMouseSnapper)
    {
        m_windowMouseSnapper->MoveSizeEnd();
        m_windowMouseSnapper = nullptr;
    }

    if (m_resizedWindow)
    {
        // Resizing a docked window resizes its neighbors
        for (const auto& [_, workArea] : m_workAreaConfiguration.GetAllWorkAreas())
        {
            if (workArea && workArea->DockResized(m_resizedWindow, m_resizedWindowRect))
            {
                break;
            }
        }

        m_resizedWindow = nullptr;
    }

    // Always disable dragging state, even if m_windowMouseSnapper was already null.
    // This prevents stuck drag state when a window is destroyed mid-drag.
    m_draggingState.Disable();
}

void FancyZones::AbortMoveSize()
{
    if (m_windowMouseSnapper)
    {
        m_windowMouseSnapper->Abort();
        m_windowMouseSnapper = nullptr;
    }

    m_draggingState.Disable();
}

bool FancyZones::MoveToAppLastZone(HWND window, HMONITOR monitor, GUID currentVirtualDesktop) noexcept
{
    const auto& workAreas = m_workAreaConfiguration.GetAllWorkAreas();
    WorkArea* workArea{ nullptr };
    ZoneIndexSet indexes{};

    if (monitor)
    {    
        if (workAreas.contains(monitor))
        {
            workArea = workAreas.at(monitor).get();
            if (workArea && workArea->UniqueId().virtualDesktopId == currentVirtualDesktop)
            {
                indexes = AppZoneHistory::instance().GetAppLastZoneIndexSet(window, workArea->UniqueId(), workArea->GetLayoutId());
            }
        }
        else
        {
            Logger::error(L"Unable to find work area for requested monitor on the active virtual desktop");
        }
    }
    else
    {
        for (const auto& [_, secondaryWorkArea] : workAreas)
        {
            if (secondaryWorkArea && secondaryWorkArea->UniqueId().virtualDesktopId == currentVirtualDesktop)
            {
                indexes = AppZoneHistory::instance().GetAppLastZoneIndexSet(window, secondaryWorkArea->UniqueId(), secondaryWorkArea->GetLayoutId());
                workArea = secondaryWorkArea.get();
                if (!indexes.empty())
                {
                    break;
                }
            }
        }
    }
    
    if (!indexes.empty() && workArea)
    {
        Trace::FancyZones::SnapNewWindowIntoZone(workArea->GetLayout().get(), workArea->GetLayoutWindows());
        workArea->Snap(window, indexes);

        return true;
    }

    return false;
}

void FancyZones::WindowCreated(HWND window) noexcept
{
    DockVisibility(window, false);
    DockClaim(window);

    // Docking replaces zones, a new window goes to the slot kept for its app instead
    const bool moveToAppLastZone = false;
    const bool openOnActiveMonitor = FancyZonesSettings::settings().openWindowOnActiveMonitor;
    if (!moveToAppLastZone && !openOnActiveMonitor)
    {
        // Nothing to do here then.
        return;
    }

    if (!FancyZonesWindowProcessing::IsProcessableAutomatically(window))
    {
        return;
    }

    // Avoid already stamped (zoned) windows
    const bool isZoned = !FancyZonesWindowProperties::RetrieveZoneIndexProperty(window).empty();
    if (isZoned)
    {
        return;
    }

    HMONITOR primary = MonitorFromWindow(nullptr, MONITOR_DEFAULTTOPRIMARY);
    HMONITOR active = primary;

    POINT cursorPosition{};
    if (GetCursorPos(&cursorPosition))
    {
        active = MonitorFromPoint(cursorPosition, MONITOR_DEFAULTTOPRIMARY);
    }

    bool windowMovedToZone = false;
    auto currentVirtualDesktop = VirtualDesktop::instance().GetCurrentVirtualDesktopIdFromRegistry();
    if (moveToAppLastZone)
    {
        if (FancyZonesSettings::settings().spanZonesAcrossMonitors)
        {
            windowMovedToZone = MoveToAppLastZone(window, nullptr, currentVirtualDesktop);
        }
        else
        {
            // Search application history on currently active monitor.
            windowMovedToZone = MoveToAppLastZone(window, active, currentVirtualDesktop);

            if (!windowMovedToZone && primary != active)
            {
                // Search application history on primary monitor.
                windowMovedToZone = MoveToAppLastZone(window, primary, currentVirtualDesktop);
            }

            if (!windowMovedToZone)
            {
                // Search application history on remaining monitors.
                windowMovedToZone = MoveToAppLastZone(window, nullptr, currentVirtualDesktop);
            }
        }
    }
    

    // Open on active monitor if window wasn't zoned
    if (openOnActiveMonitor && !windowMovedToZone)
    {
        // window is recreated after switching virtual desktop
        // avoid moving already opened windows after switching vd
        bool isMoved = FancyZonesWindowProperties::RetrieveMovedOnOpeningProperty(window);
        if (!isMoved)
        {
            FancyZonesWindowProperties::StampMovedOnOpeningProperty(window);
            m_dpiUnawareThread.submit(OnThreadExecutor::task_t{ [&] { MonitorUtils::OpenWindowOnActiveMonitor(window, active); } }).wait();
        }
    }
}

// IFancyZonesCallback
IFACEMETHODIMP_(bool)
FancyZones::OnKeyDown(PKBDLLHOOKSTRUCT info) noexcept
{
    m_monitorRotationKeyState.Update(info->vkCode, true);

    // Return true to swallow the keyboard event
    bool const shift = GetAsyncKeyState(VK_SHIFT) & 0x8000;
    bool const win = GetAsyncKeyState(VK_LWIN) & 0x8000 || GetAsyncKeyState(VK_RWIN) & 0x8000;
    bool const alt = GetAsyncKeyState(VK_MENU) & 0x8000;
    bool const ctrl = GetAsyncKeyState(VK_CONTROL) & 0x8000;
    if (IsMonitorRotationChordDown())
    {
        if (!m_monitorRotationPreviewActive)
        {
            m_monitorRotationPreviewActive = true;
            PostMessageW(m_window, WM_PRIV_MONITOR_ROTATION_PREVIEW_SHOW, 0, 0);
        }

        if (info->vkCode == VK_LEFT || info->vkCode == VK_RIGHT)
        {
            if (!m_pendingMonitorRotationReverse.has_value())
            {
                const bool reverse = info->vkCode == VK_LEFT;
                PostMessageW(m_window, WM_PRIV_MONITOR_ROTATION_PREVIEW_ROTATE, static_cast<WPARAM>(reverse), 0);
            }

            m_monitorRotationKeyState.Consume(info->vkCode);
            return true;
        }

        if (IsMonitorRotationActivatorKey(info->vkCode))
        {
            m_monitorRotationKeyState.Consume(info->vkCode);
            return true;
        }

        if (IsRotationPreviewReleaseKey(info->vkCode))
        {
            return false;
        }
    }
    else if (m_monitorRotationPreviewActive)
    {
        m_monitorRotationPreviewActive = false;
        PostMessageW(m_window, WM_PRIV_MONITOR_ROTATION_PREVIEW_HIDE, 0, 0);
    }

    // Win+arrows move the foreground window in the dock layout
    if (win && !shift && !ctrl && !alt && FancyZonesSettings::settings().overrideSnapHotkeys)
    {
        if (info->vkCode == VK_LEFT || info->vkCode == VK_RIGHT || info->vkCode == VK_UP || info->vkCode == VK_DOWN)
        {
            if (FancyZonesWindowProcessing::IsProcessableManually(GetForegroundWindow()))
            {
                PostMessageW(m_window, WM_PRIV_SNAP_HOTKEY, 0, info->vkCode);
                return true;
            }
        }
    }

    if (win && ctrl && alt)
    {
        const int digit = DigitFromVkCode(info->vkCode);
        // Win+Ctrl+Alt+Shift+digit saves the dock layout, Win+Ctrl+Alt+digit recalls it.
        // The digit isn't swallowed when there is nothing to recall.
        if (digit != -1 && (shift || DockLayouts::instance().HasSlot(digit)))
        {
            // Not while a window is dragged (Shift is held to dock), and once per key press
            if (!m_windowMouseSnapper && m_dockSlotKey != info->vkCode)
            {
                m_dockSlotKey = info->vkCode;
                PostMessageW(m_window, WM_PRIV_DOCK_SLOT, shift, digit);
            }

            return true;
        }
    }

    if (FancyZonesSettings::settings().quickLayoutSwitch)
    {
        int digitPressed = -1;
        if ('0' <= info->vkCode && info->vkCode <= '9')
        {
            digitPressed = info->vkCode - '0';
        }
        else if (VK_NUMPAD0 <= info->vkCode && info->vkCode <= VK_NUMPAD9)
        {
            digitPressed = info->vkCode - VK_NUMPAD0;
        }

        bool dragging = m_draggingState.IsDragging();
        bool changeLayoutWhileNotDragging = !dragging && !shift && win && ctrl && alt && digitPressed != -1;
        // Require Win+Ctrl+Alt even while dragging to prevent accidental layout switches
        // when drag state is stuck (root cause of #410 "steals number keys")
        bool changeLayoutWhileDragging = dragging && win && ctrl && alt && digitPressed != -1;

        if (changeLayoutWhileNotDragging || changeLayoutWhileDragging)
        {
            auto layoutId = LayoutHotkeys::instance().GetLayoutId(digitPressed);
            if (layoutId.has_value())
            {
                PostMessageW(m_window, WM_PRIV_QUICK_LAYOUT_KEY, 0, static_cast<LPARAM>(digitPressed));
                Trace::FancyZones::QuickLayoutSwitched(changeLayoutWhileNotDragging);
                return true;
            }
        }
    }

    // Only suppress the bare Shift key itself during drag (used for drag-toggle).
    // Do NOT swallow Shift+<other key> combos - that steals keystrokes from apps.
    if (m_windowMouseSnapper &&
        (info->vkCode == VK_LSHIFT || info->vkCode == VK_RSHIFT))
    {
        // Record the press before swallowing it. Returning 1 removes the key from the input stream
        // for every listener - including this module's own WM_INPUT handler, which is what normally
        // calls SetShiftState - so without this the zones could never be switched off with Shift.
        m_draggingState.SetShiftState(true);
        return true;
    }
    return false;
}

IFACEMETHODIMP_(bool)
FancyZones::OnKeyUp(PKBDLLHOOKSTRUCT info) noexcept
{
    const bool wasMonitorRotationPreviewActive = m_monitorRotationPreviewActive;
    const bool isActivatorKey = IsMonitorRotationActivatorKey(info->vkCode);
    const bool wasConsumed = m_monitorRotationKeyState.ReleaseWasConsumed(info->vkCode);
    if (info->vkCode == m_dockSlotKey)
    {
        m_dockSlotKey = 0;
    }
    m_monitorRotationKeyState.Update(info->vkCode, false);

    if (wasMonitorRotationPreviewActive && !IsMonitorRotationChordDown())
    {
        m_monitorRotationPreviewActive = false;
        PostMessageW(m_window, WM_PRIV_MONITOR_ROTATION_PREVIEW_HIDE, 0, 0);
        return isActivatorKey || wasConsumed;
    }

    return wasConsumed;
}

bool FancyZones::IsMonitorRotationChordDown() const noexcept
{
    if (!FancyZonesSettings::settings().monitorRotation)
    {
        return false;
    }

    const auto& hotkey = FancyZonesSettings::settings().monitorRotationHotkey;
    const bool winDown = m_monitorRotationKeyState.IsAnyDown({ VK_LWIN, VK_RWIN });
    const bool ctrlDown = m_monitorRotationKeyState.IsAnyDown({ VK_CONTROL, VK_LCONTROL, VK_RCONTROL });
    const bool altDown = m_monitorRotationKeyState.IsAnyDown({ VK_MENU, VK_LMENU, VK_RMENU });
    const bool shiftDown = m_monitorRotationKeyState.IsAnyDown({ VK_SHIFT, VK_LSHIFT, VK_RSHIFT });
    return hotkey.win_pressed() == winDown &&
           hotkey.ctrl_pressed() == ctrlDown &&
           hotkey.alt_pressed() == altDown &&
           hotkey.shift_pressed() == shiftDown &&
           m_monitorRotationKeyState.IsDown(hotkey.get_code());
}

bool FancyZones::IsMonitorRotationActivatorKey(DWORD vkCode) const noexcept
{
    return vkCode == FancyZonesSettings::settings().monitorRotationHotkey.get_code();
}

void FancyZones::ToggleEditor() noexcept
{
    _TRACER_;

    if (m_terminateEditorEvent)
    {
        SetEvent(m_terminateEditorEvent.get());
        return;
    }

    m_terminateEditorEvent.reset(CreateEvent(nullptr, true, false, nullptr));

    if (!EditorParameters::Save(m_workAreaConfiguration, m_dpiUnawareThread))
    {
        Logger::error(L"Failed to save editor startup parameters");
        return;
    }

    SHELLEXECUTEINFO sei{ sizeof(sei) };
    sei.fMask = { SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI };
    sei.lpFile = NonLocalizable::FZEditorExecutablePath;
    sei.lpParameters = L"";
    sei.nShow = SW_SHOWDEFAULT;
    ShellExecuteEx(&sei);
    Trace::FancyZones::EditorLaunched(1);

    // Launch the editor on a background thread
    // Wait for the editor's process to exit
    // Post back to the main thread to update
    std::thread waitForEditorThread([window = m_window, processHandle = sei.hProcess, terminateEditorEvent = m_terminateEditorEvent.get()]() {
        HANDLE waitEvents[2] = { processHandle, terminateEditorEvent };
        auto result = WaitForMultipleObjects(2, waitEvents, false, INFINITE);
        if (result == WAIT_OBJECT_0 + 0)
        {
            // Editor exited
            // Update any changes it may have made
            PostMessage(window, WM_PRIV_EDITOR, 0, static_cast<LPARAM>(EditorExitKind::Exit));
        }
        else if (result == WAIT_OBJECT_0 + 1)
        {
            // User hit Win+~ while editor is already running
            // Shut it down
            TerminateProcess(processHandle, 2);
            PostMessage(window, WM_PRIV_EDITOR, 0, static_cast<LPARAM>(EditorExitKind::Terminate));
        }
        CloseHandle(processHandle);
    });

    waitForEditorThread.detach();
}

LRESULT FancyZones::WndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept
{
    switch (message)
    {
    case WM_QUERYENDSESSION:
        return TRUE;

    case WM_ENDSESSION:
        if (wparam)
        {
            // This window has no WM_DESTROY -> PostQuitMessage path, so it
            // cannot use handle_stateless_session_end_message.
            PostQuitMessage(0);
        }
        return 0;

    case WM_HOTKEY:
    {
        if (wparam == static_cast<WPARAM>(HotkeyId::Editor))
        {
            ToggleEditor();
        }
        else if (wparam == static_cast<WPARAM>(HotkeyId::NextTab) || wparam == static_cast<WPARAM>(HotkeyId::PrevTab))
        {
            bool reverse = wparam == static_cast<WPARAM>(HotkeyId::PrevTab);
            CycleWindows(reverse);
        }
    }
    break;

    case WM_INPUT:
    {
        OnKeyboardInput(wparam, reinterpret_cast<HRAWINPUT>(lparam));
    }
    break;

    case WM_SETTINGCHANGE:
    {
        if (wparam == SPI_SETWORKAREA)
        {
            // Changes in taskbar position resulted in different size of work area.
            // Invalidate cached work-areas so they can be recreated with latest information.
            OnDisplayChange(DisplayChangeType::WorkArea);
        }
    }
    break;

    case WM_DISPLAYCHANGE:
    {
        // Display resolution changed. Invalidate cached work-areas so they can be recreated with latest information.
        OnDisplayChange(DisplayChangeType::DisplayChange);
    }
    break;

    default:
    {
        POINT ptScreen;
        GetPhysicalCursorPos(&ptScreen);

        if (message == WM_PRIV_SNAP_HOTKEY)
        {
            DockMoveByKey(static_cast<DWORD>(lparam));
        }
        else if (message == WM_PRIV_INIT)
        {
            OnDisplayChange(DisplayChangeType::Initialization);
        }
        else if (message == WM_PRIV_VD_SWITCH)
        {
            OnDisplayChange(DisplayChangeType::VirtualDesktop);
        }
        else if (message == WM_PRIV_EDITOR)
        {
            // Clean up the event either way
            m_terminateEditorEvent.release();
        }
        else if (message == WM_PRIV_MOVESIZESTART)
        {
            auto hwnd = reinterpret_cast<HWND>(wparam);
            if (auto monitor = MonitorFromPoint(ptScreen, MONITOR_DEFAULTTONULL))
            {
                MoveSizeStart(hwnd, monitor);
                MoveSizeUpdate(monitor, ptScreen);
            }
        }
        else if (message == WM_PRIV_MOVESIZEEND)
        {
            MoveSizeEnd();
        }
        else if (message == WM_PRIV_LOCATIONCHANGE)
        {
            if (auto monitor = MonitorFromPoint(ptScreen, MONITOR_DEFAULTTONULL))
            {
                MoveSizeUpdate(monitor, ptScreen);
            }
        }
        else if (message == WM_PRIV_WINDOWCREATED)
        {
            auto hwnd = reinterpret_cast<HWND>(wparam);
            WindowCreated(hwnd);
        }
        else if (message == WM_PRIV_WINDOWDESTROYED)
        {
            auto hwnd = reinterpret_cast<HWND>(wparam);
            // If the destroyed window was being dragged, abort the drag without
            // snapping. Calling MoveSizeEnd() here would snap the now-destroyed
            // HWND into a zone and corrupt the layout state.
            if (m_windowMouseSnapper && m_windowMouseSnapper->GetDraggedWindow() == hwnd)
            {
                Logger::info(L"Window destroyed during drag - aborting drag");
                AbortMoveSize();
            }

            if (m_resizedWindow == hwnd)
            {
                m_resizedWindow = nullptr;
            }

            // The window can be docked on a virtual desktop that isn't the current one
            if (DockLayouts::instance().RemoveWindow(hwnd))
            {
                for (const auto& [_, workArea] : m_workAreaConfiguration.GetAllWorkAreas())
                {
                    if (workArea)
                    {
                        workArea->ApplyDockLayout();
                    }
                }
            }
        }
        else if (message == WM_PRIV_DOCK_VISIBILITY)
        {
            DockVisibility(reinterpret_cast<HWND>(wparam), lparam != 0);
        }
        else if (message == WM_PRIV_DOCK_SLOT)
        {
            DockSlot(static_cast<int>(lparam), wparam != 0);
        }
        else if (message == WM_PRIV_LAYOUT_HOTKEYS_FILE_UPDATE)
        {
            LayoutHotkeys::instance().LoadData();
        }
        else if (message == WM_PRIV_LAYOUT_TEMPLATES_FILE_UPDATE)
        {
            LayoutTemplates::instance().LoadData();
        }
        else if (message == WM_PRIV_CUSTOM_LAYOUTS_FILE_UPDATE)
        {
            CustomLayouts::instance().LoadData();
            RefreshLayouts();
        }
        else if (message == WM_PRIV_APPLIED_LAYOUTS_FILE_UPDATE)
        {
            AppliedLayouts::instance().LoadData();
            RefreshLayouts();
        }
        else if (message == WM_PRIV_DEFAULT_LAYOUTS_FILE_UPDATE)
        {
            DefaultLayouts::instance().LoadData();
        }
        else if (message == WM_PRIV_QUICK_LAYOUT_KEY)
        {
            ApplyQuickLayout(static_cast<int>(lparam));
        }
        else if (message == WM_PRIV_WHEEL_LAYOUT_SWITCH)
        {
            // wheel up cycles backwards, wheel down cycles forward
            CycleLayoutByWheel(wparam != 0);
        }
        else if (message == WM_PRIV_MONITOR_ROTATION_PREVIEW_SHOW)
        {
            ShowMonitorRotationPreview();
        }
        else if (message == WM_PRIV_MONITOR_ROTATION_PREVIEW_HIDE)
        {
            HideMonitorRotationPreview();
        }
        else if (message == WM_PRIV_MONITOR_ROTATION_PREVIEW_ROTATE)
        {
            const bool reverse = static_cast<bool>(wparam);
            m_pendingMonitorRotationReverse = reverse;
            ShowMonitorRotationPreview(reverse, true);
            if (SetTimer(m_window, MonitorRotationCommitTimerId, MonitorRotationCommitDelayMillis, nullptr) == 0)
            {
                m_pendingMonitorRotationReverse.reset();
                ShowMonitorRotationPreview();
            }
        }
        else if (message == WM_TIMER && wparam == MonitorRotationCommitTimerId)
        {
            KillTimer(m_window, MonitorRotationCommitTimerId);
            if (m_pendingMonitorRotationReverse.has_value())
            {
                const bool reverse = *m_pendingMonitorRotationReverse;
                const bool shouldShowCommitPreview = m_monitorRotationPreviewActive && IsMonitorRotationChordDown();
                m_pendingMonitorRotationReverse.reset();
                RotateWindowsAcrossMonitors(reverse);
                RotateMonitorRotationContentNumbers(reverse);
                if (shouldShowCommitPreview)
                {
                    ShowMonitorRotationPreview();
                }
            }
        }
        else if (message == WM_PRIV_SETTINGS_CHANGED)
        {
            FancyZonesSettings::instance().LoadSettings();
        }
        else if (message == WM_PRIV_SAVE_EDITOR_PARAMETERS)
        {
            if (!EditorParameters::Save(m_workAreaConfiguration, m_dpiUnawareThread))
            {
                Logger::warn(L"Failed to save editor-parameters.json");
            }
        }
        else
        {
            return DefWindowProc(window, message, wparam, lparam);
        }
    }
    break;
    }
    return 0;
}

void FancyZones::OnKeyboardInput(WPARAM /*flags*/, HRAWINPUT hInput) noexcept
{
    auto input = KeyboardInput::OnKeyboardInput(hInput);
    if (!input.has_value())
    {
        return;
    }

    switch (input.value().vkKey)
    {
    case VK_SHIFT:
        {
            m_draggingState.SetShiftState(input.value().pressed);
        }
        break;
    default:
        break;
    }
}

void FancyZones::OnDisplayChange(DisplayChangeType changeType) noexcept
{
    Logger::info(L"Display changed, type: {}", DisplayChangeTypeName(changeType));

    bool updateWindowsPositions = false;

    switch (changeType)
    {
    case DisplayChangeType::WorkArea: // WorkArea size changed
    case DisplayChangeType::DisplayChange: // Resolution changed or display added
        updateWindowsPositions = FancyZonesSettings::settings().displayOrWorkAreaChange_moveWindows;
        break;
    case DisplayChangeType::VirtualDesktop: // Switched virtual desktop
        SyncVirtualDesktops();
        break;
    case DisplayChangeType::Initialization: // Initialization
        updateWindowsPositions = FancyZonesSettings::settings().zoneSetChange_moveWindows;
        break;
    default:
        break;
    }

    UpdateWorkAreas(updateWindowsPositions);
}

bool FancyZones::AddWorkArea(HMONITOR monitor, const FancyZonesDataTypes::WorkAreaId& id, const FancyZonesUtils::Rect& rect) noexcept
{
    auto virtualDesktopIdStr = FancyZonesUtils::GuidToString(id.virtualDesktopId);
    if (virtualDesktopIdStr)
    {
        Logger::debug(L"Add new work area on virtual desktop {}", virtualDesktopIdStr.value());
    }

    auto parentWorkAreaId = id;
    parentWorkAreaId.virtualDesktopId = LastUsedVirtualDesktop::instance().GetId();

    auto workArea = WorkArea::Create(m_hinstance, id, parentWorkAreaId, rect);
    if (!workArea)
    {
        Logger::error(L"Failed to create work area {}", id.toString());
        return false;
    }
    
    m_workAreaConfiguration.AddWorkArea(monitor, std::move(workArea));
    return true;
}

LRESULT CALLBACK FancyZones::s_WndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept
{
    auto thisRef = reinterpret_cast<FancyZones*>(GetWindowLongPtr(window, GWLP_USERDATA));
    if (!thisRef && (message == WM_CREATE))
    {
        const auto createStruct = reinterpret_cast<LPCREATESTRUCT>(lparam);
        thisRef = static_cast<FancyZones*>(createStruct->lpCreateParams);
        SetWindowLongPtr(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(thisRef));
    }

    return thisRef ? thisRef->WndProc(window, message, wparam, lparam) :
                     DefWindowProc(window, message, wparam, lparam);
}

void FancyZones::UpdateWorkAreas(bool updateWindowPositions) noexcept
{
    Logger::debug(L"Update work areas, update windows positions: {}", updateWindowPositions);

    auto currentVirtualDesktop = VirtualDesktop::instance().GetCurrentVirtualDesktopIdFromRegistry();

    if (FancyZonesSettings::settings().spanZonesAcrossMonitors)
    {
        std::vector<FancyZonesDataTypes::MonitorId> monitors = { FancyZonesDataTypes::MonitorId{ .monitor = nullptr, .deviceId = { .id = ZonedWindowProperties::MultiMonitorName, .instanceId = ZonedWindowProperties::MultiMonitorInstance } } };
        if (ShouldWorkAreasBeRecreated(monitors, currentVirtualDesktop, m_workAreaConfiguration.GetAllWorkAreas()))
        {
            // WindowMouseSnap caches a raw WorkArea* in m_currentWorkArea and the
            // WorkArea map by reference. WorkAreaConfiguration::Clear() destroys
            // every unique_ptr<WorkArea> (and hence the inner ZonesOverlay and
            // its std::mutex). If a drag is in flight, the next MoveSizeUpdate
            // would dereference that dangling WorkArea* and lock the freed
            // mutex. Abort the active drag first so subsequent drag messages
            // hit the snapper's `if (m_windowMouseSnapper)` guard and no-op.
            AbortMoveSize();
            m_workAreaConfiguration.Clear();

            FancyZonesDataTypes::WorkAreaId workAreaId;
            workAreaId.virtualDesktopId = currentVirtualDesktop;
            workAreaId.monitorId = { .deviceId = { .id = ZonedWindowProperties::MultiMonitorName, .instanceId = ZonedWindowProperties::MultiMonitorInstance } };

            AddWorkArea(nullptr, workAreaId, FancyZonesUtils::GetAllMonitorsCombinedRect<&MONITORINFO::rcWork>());
        }
    }
    else
    {
        auto monitors = MonitorUtils::IdentifyMonitors();
        const auto& workAreas = m_workAreaConfiguration.GetAllWorkAreas();
        
        if (ShouldWorkAreasBeRecreated(monitors, currentVirtualDesktop, workAreas))
        {
            // See comment above the matching Clear() in the span-zones branch.
            AbortMoveSize();
            m_workAreaConfiguration.Clear();
            for (const auto& monitor : monitors)
            {
                FancyZonesDataTypes::WorkAreaId workAreaId;
                workAreaId.virtualDesktopId = currentVirtualDesktop;
                workAreaId.monitorId = monitor;

                AddWorkArea(monitor.monitor, workAreaId, MonitorUtils::GetWorkAreaRect(monitor.monitor));
            }
        }
    }

    // init previously snapped windows
    std::unordered_map<HWND, ZoneIndexSet> windowsToSnap{};
    for (const auto& window : VirtualDesktop::instance().GetWindowsFromCurrentDesktop())
    {
        auto indexes = FancyZonesWindowProperties::RetrieveZoneIndexProperty(window);
        if (indexes.size() == 0)
        {
            continue;
        }

        windowsToSnap.insert({ window, indexes });
    }

    if (FancyZonesSettings::settings().spanZonesAcrossMonitors) // one work area across monitors
    {
        const auto workArea = m_workAreaConfiguration.GetWorkArea(nullptr);
        if (workArea)
        {
            for (const auto& [window, zones] : windowsToSnap)
            {
                workArea->Snap(window, zones, false);
            }
        }
    }
    else
    {
        // first, snap windows to the monitor where they're placed
        for (auto iter = windowsToSnap.begin(); iter != windowsToSnap.end();)
        {
            const auto window = iter->first;
            const auto zones = iter->second;
            const auto monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONULL);
            const auto workAreaForMonitor = m_workAreaConfiguration.GetWorkArea(monitor);
            if (workAreaForMonitor && AppZoneHistory::instance().GetAppLastZoneIndexSet(window, workAreaForMonitor->UniqueId(), workAreaForMonitor->GetLayoutId()) == zones)
            {
                workAreaForMonitor->Snap(window, zones, false);
                iter = windowsToSnap.erase(iter);
            }
            else
            {
                ++iter;
            }
        }

        // snap rest of the windows to other work areas (in case they were moved after the monitor unplug)
        for (const auto& [window, zones] : windowsToSnap)
        {
            for (const auto& [_, workArea] : m_workAreaConfiguration.GetAllWorkAreas())
            {
                const auto savedIndexes = AppZoneHistory::instance().GetAppLastZoneIndexSet(window, workArea->UniqueId(), workArea->GetLayoutId());
                if (savedIndexes == zones)
                {
                    workArea->Snap(window, zones, false);
                }
            }
        }
    }

    // The layouts weren't kept up to date while their virtual desktop wasn't the current one
    for (const auto& [_, workArea] : m_workAreaConfiguration.GetAllWorkAreas())
    {
        if (workArea)
        {
            DockLayouts::Refresh(workArea->DockTree());
        }
    }

    // Windows opened while FancyZones wasn't running take the slots kept for their apps
    for (const auto& window : VirtualDesktop::instance().GetWindowsFromCurrentDesktop())
    {
        DockClaim(window);
    }

    for (const auto& [_, workArea] : m_workAreaConfiguration.GetAllWorkAreas())
    {
        if (workArea)
        {
            workArea->ApplyDockLayout();
        }
    }
}

bool FancyZones::ShouldWorkAreasBeRecreated(const std::vector<FancyZonesDataTypes::MonitorId>& monitors, const GUID& virtualDesktop, const std::unordered_map<HMONITOR, std::unique_ptr<WorkArea>>& workAreas) noexcept
{
    if (monitors.size() != workAreas.size())
    {
        Logger::trace(L"Monitor was added or removed");
        return true;
    }

    for (const auto& monitor : monitors)
    {
        auto iter = workAreas.find(monitor.monitor);
        if (iter == workAreas.end())
        {
            Logger::trace(L"WorkArea not found");
            return true;
        }

        if (iter->second->UniqueId().monitorId.deviceId != monitor.deviceId)
        {
            Logger::trace(L"DeviceId changed");
            return true;
        }

        if (iter->second->UniqueId().monitorId.serialNumber != monitor.serialNumber)
        {
            Logger::trace(L"Serial number changed");
            return true;
        }

        if (iter->second->UniqueId().virtualDesktopId != virtualDesktop)
        {
            Logger::trace(L"Virtual desktop changed");
            return true;
        }

        const auto rect = monitor.monitor ? MonitorUtils::GetWorkAreaRect(monitor.monitor) : FancyZonesUtils::Rect(FancyZonesUtils::GetMonitorsCombinedRect<&MONITORINFOEX::rcWork>(FancyZonesUtils::GetAllMonitorRects<&MONITORINFOEX::rcWork>()));
        if (iter->second->GetWorkAreaRect() != rect)
        {
            Logger::trace(L"WorkArea size changed");
            return true;
        }
    }

    return false;
}

void FancyZones::CycleWindows(bool reverse) noexcept
{
    auto window = GetForegroundWindow();
    HMONITOR current = WorkAreaKeyFromWindow(window);

    auto workArea = m_workAreaConfiguration.GetWorkArea(current);
    if (workArea)
    {
        workArea->CycleWindows(window, reverse);
    }
}

void FancyZones::ShowMonitorRotationPreview(std::optional<bool> reverse, bool animateRotation) noexcept
{
    auto windows = CollectWindowRotationSnapshots();
    std::unordered_map<HMONITOR, std::vector<RECT>> windowRectsByMonitor;
    for (const auto& window : windows)
    {
        windowRectsByMonitor[window.sourceMonitor].push_back(window.sourceRect);
    }

    auto monitors = FancyZonesUtils::GetAllMonitorRects<&MONITORINFOEX::rcWork>();
    FancyZonesUtils::OrderMonitors(monitors);
    EnsureMonitorRotationContentNumbers(monitors);

    if (FancyZonesSettings::settings().spanZonesAcrossMonitors)
    {
        auto workArea = m_workAreaConfiguration.GetWorkArea(nullptr);
        if (workArea)
        {
            std::vector<RECT> allWindowRects;
            allWindowRects.reserve(windows.size());
            for (const auto& window : windows)
            {
                allWindowRects.push_back(window.sourceRect);
            }

            workArea->ShowMonitorRotationPreview(allWindowRects, 1, reverse, animateRotation);
        }

        m_monitorRotationPreviewActive = true;
        return;
    }

    for (const auto& [monitor, workArea] : m_workAreaConfiguration.GetAllWorkAreas())
    {
        if (!workArea)
        {
            continue;
        }

        const auto iter = windowRectsByMonitor.find(monitor);
        const std::vector<RECT> emptyRects;
        const auto monitorNumberIter = m_monitorRotationContentNumbers.find(monitor);
        const size_t monitorNumber = monitorNumberIter != m_monitorRotationContentNumbers.end() ? monitorNumberIter->second : GetMonitorDisplayNumber(monitor, 1);
        workArea->ShowMonitorRotationPreview(iter != windowRectsByMonitor.end() ? iter->second : emptyRects, monitorNumber, reverse, animateRotation);
    }

    m_monitorRotationPreviewActive = true;
}

void FancyZones::EnsureMonitorRotationContentNumbers(const std::vector<std::pair<HMONITOR, RECT>>& monitors) noexcept
{
    if (monitors.empty())
    {
        m_monitorRotationContentNumbers.clear();
        return;
    }

    bool shouldReinitialize = m_monitorRotationContentNumbers.size() != monitors.size();
    if (!shouldReinitialize)
    {
        for (const auto& [monitor, _] : monitors)
        {
            if (!m_monitorRotationContentNumbers.contains(monitor))
            {
                shouldReinitialize = true;
                break;
            }
        }
    }

    if (!shouldReinitialize)
    {
        return;
    }

    m_monitorRotationContentNumbers.clear();
    for (size_t monitorIndex = 0; monitorIndex < monitors.size(); monitorIndex++)
    {
        m_monitorRotationContentNumbers[monitors[monitorIndex].first] = GetMonitorDisplayNumber(monitors[monitorIndex].first, monitorIndex + 1);
    }
}

void FancyZones::RotateMonitorRotationContentNumbers(bool reverse) noexcept
{
    auto monitors = FancyZonesUtils::GetAllMonitorRects<&MONITORINFOEX::rcWork>();
    FancyZonesUtils::OrderMonitors(monitors);
    EnsureMonitorRotationContentNumbers(monitors);

    if (monitors.size() < 2)
    {
        return;
    }

    std::unordered_map<HMONITOR, size_t> rotatedContentNumbers;
    for (size_t sourceIndex = 0; sourceIndex < monitors.size(); sourceIndex++)
    {
        const size_t targetIndex = MonitorRotation::GetRotatedMonitorIndex(sourceIndex, monitors.size(), reverse);
        rotatedContentNumbers[monitors[targetIndex].first] = m_monitorRotationContentNumbers[monitors[sourceIndex].first];
    }

    m_monitorRotationContentNumbers = std::move(rotatedContentNumbers);
}

void FancyZones::HideMonitorRotationPreview() noexcept
{
    for (const auto& [_, workArea] : m_workAreaConfiguration.GetAllWorkAreas())
    {
        if (workArea)
        {
            workArea->HideZones();
        }
    }

    m_monitorRotationPreviewActive = false;
    if (!m_pendingMonitorRotationReverse.has_value())
    {
        KillTimer(m_window, MonitorRotationCommitTimerId);
    }
}

void FancyZones::RotateWindowsAcrossMonitors(bool reverse) noexcept
{
    auto monitors = FancyZonesUtils::GetAllMonitorRects<&MONITORINFOEX::rcWork>();
    FancyZonesUtils::OrderMonitors(monitors);
    if (monitors.size() < 2)
    {
        Logger::info(L"Monitor rotation skipped: fewer than two monitors are available");
        return;
    }

    auto windows = CollectWindowRotationSnapshots();
    if (windows.empty())
    {
        Logger::info(L"Monitor rotation skipped: no processable windows found");
        return;
    }

    size_t movedWindows = 0;
    for (const auto& window : windows)
    {
        const auto sourceIndex = FindMonitorIndex(window.sourceMonitor, monitors);
        if (!sourceIndex.has_value())
        {
            continue;
        }

        const size_t targetIndex = MonitorRotation::GetRotatedMonitorIndex(*sourceIndex, monitors.size(), reverse);
        const RECT targetRect = MonitorRotation::MapRectBetweenMonitorWorkAreas(
            window.sourceRect,
            monitors[*sourceIndex].second,
            monitors[targetIndex].second);
        if (!FancyZonesWindowProperties::RetrieveZoneIndexProperty(window.window).empty())
        {
            if (auto workArea = m_workAreaConfiguration.GetWorkAreaFromWindow(window.window))
            {
                workArea->Unsnap(window.window);
            }
        }

        FancyZonesWindowUtils::SizeWindowToRect(window.window, targetRect, false);
        movedWindows++;
    }

    Logger::info(L"Rotated {} windows across {} monitors", movedWindows, monitors.size());
}

void FancyZones::SyncVirtualDesktops() noexcept
{
    // Explorer persists current virtual desktop identifier to registry on a per session basis,
    // but only after first virtual desktop switch happens. If the user hasn't switched virtual
    // desktops in this session value in registry will be empty and we will use default GUID in
    // that case (00000000-0000-0000-0000-000000000000).

    auto lastUsed = LastUsedVirtualDesktop::instance().GetId();
    auto current = VirtualDesktop::instance().GetCurrentVirtualDesktopIdFromRegistry();
    auto guids = VirtualDesktop::instance().GetVirtualDesktopIdsFromRegistry();

    if (current != lastUsed)
    {
        LastUsedVirtualDesktop::instance().SetId(current);
        LastUsedVirtualDesktop::instance().SaveData();
    }

    AppliedLayouts::instance().SyncVirtualDesktops(current, lastUsed, guids);
    AppZoneHistory::instance().SyncVirtualDesktops(current, lastUsed, guids);
}

void FancyZones::UpdateHotkey(int hotkeyId, const PowerToysSettings::HotkeyObject& hotkeyObject, bool enable) noexcept
{
    if (!m_window)
    {
        return;
    }

    UnregisterHotKey(m_window, hotkeyId);

    if (!enable)
    {
        return;
    }

    auto modifiers = hotkeyObject.get_modifiers();
    auto code = hotkeyObject.get_code();
    auto result = RegisterHotKey(m_window, hotkeyId, modifiers, code);

    if (!result)
    {
        Logger::error(L"Failed to register hotkey: {}", get_last_error_or_default(GetLastError()));
    }
}

void FancyZones::SettingsUpdate(SettingId id)
{
    switch (id)
    {
    case SettingId::EditorHotkey:
    {
        UpdateHotkey(static_cast<int>(HotkeyId::Editor), FancyZonesSettings::settings().editorHotkey, true);
    }
    break;
    case SettingId::WindowSwitching:
    {
        UpdateHotkey(static_cast<int>(HotkeyId::PrevTab), FancyZonesSettings::settings().prevTabHotkey, FancyZonesSettings::settings().windowSwitching);
        UpdateHotkey(static_cast<int>(HotkeyId::NextTab), FancyZonesSettings::settings().nextTabHotkey, FancyZonesSettings::settings().windowSwitching);
    }
    break;
    case SettingId::PrevTabHotkey:
    {
        UpdateHotkey(static_cast<int>(HotkeyId::PrevTab), FancyZonesSettings::settings().prevTabHotkey, FancyZonesSettings::settings().windowSwitching);
    }
    break;
    case SettingId::NextTabHotkey:
    {
        UpdateHotkey(static_cast<int>(HotkeyId::NextTab), FancyZonesSettings::settings().nextTabHotkey, FancyZonesSettings::settings().windowSwitching);
    }
    break;
    case SettingId::MonitorRotation:
    case SettingId::MonitorRotationHotkey:
    {
        m_pendingMonitorRotationReverse.reset();
        KillTimer(m_window, MonitorRotationCommitTimerId);
        m_monitorRotationKeyState.Reset();
        if (m_monitorRotationPreviewActive && !IsMonitorRotationChordDown())
        {
            HideMonitorRotationPreview();
        }
    }
    break;
    case SettingId::SpanZonesAcrossMonitors:
    {
        // See UpdateWorkAreas() - same WindowMouseSnap dangling-WorkArea*
        // hazard if the user toggles this setting mid-drag.
        AbortMoveSize();
        m_workAreaConfiguration.Clear();
        PostMessageW(m_window, WM_PRIV_INIT, NULL, NULL);
    }
    break;
    default:
        break;
    }
}

void FancyZones::RefreshLayouts() noexcept
{
    for (const auto& [_, workArea] : m_workAreaConfiguration.GetAllWorkAreas())
    {
        if (workArea)
        {
            workArea->InitLayout();

            if (FancyZonesSettings::settings().zoneSetChange_moveWindows)
            {
                workArea->UpdateWindowPositions();
            }
        }
    }
}

void FancyZones::ApplyQuickLayout(int key) noexcept
{
    auto layoutId = LayoutHotkeys::instance().GetLayoutId(key);
    if (!layoutId)
    {
        return;
    }

    // Find a custom zone set with this uuid and apply it
    auto layout = CustomLayouts::instance().GetLayout(layoutId.value());
    if (!layout)
    {
        return;
    }

    auto workArea = m_workAreaConfiguration.GetWorkAreaFromCursor();
    if (workArea)
    {
        SwitchLayout(workArea, layout.value());
    }
}

void FancyZones::CycleLayoutByWheel(bool reverse) noexcept
{
    auto workArea = m_workAreaConfiguration.GetWorkAreaFromCursor();
    if (!workArea)
    {
        return;
    }

    const auto layout = NextWheelLayout(*workArea, reverse);
    if (layout.has_value())
    {
        SwitchLayout(workArea, layout.value());
    }
}

// The layout the wheel would switch the work area to, or nothing when no switch is possible
std::optional<LayoutData> FancyZones::NextWheelLayout(const WorkArea& workArea, bool reverse) const noexcept
{
    const auto ids = WheelCycleLayoutIds();
    if (ids.size() < 2)
    {
        return std::nullopt;
    }

    std::optional<GUID> currentId{};
    if (const auto current = AppliedLayouts::instance().GetDeviceLayout(workArea.UniqueId()); current.has_value())
    {
        currentId = current->uuid;
    }

    const auto nextId = FancyZonesUtils::PickNextLayoutId(ids, currentId, reverse);
    if (!nextId.has_value())
    {
        return std::nullopt;
    }

    return CustomLayouts::instance().GetLayout(nextId.value());
}

// Applies a custom layout picked by a quick switch (hotkey or wheel) to the given work area
void FancyZones::SwitchLayout(WorkArea* workArea, const LayoutData& layout) noexcept
{
    if (!AppliedLayouts::instance().ApplyLayout(workArea->UniqueId(), layout))
    {
        return;
    }

    RefreshLayouts();

    // While dragging, the highlighted zones would keep reflecting the previous layout until the
    // pointer moves, so recompute them against the new layout at the current cursor position
    POINT ptScreen{};
    if (m_windowMouseSnapper && GetPhysicalCursorPos(&ptScreen))
    {
        if (auto monitor = MonitorFromPoint(ptScreen, MONITOR_DEFAULTTONULL))
        {
            MoveSizeUpdate(monitor, ptScreen);
        }
    }

    FlashZones();
    AppliedLayouts::instance().SaveData();

    if (const auto layoutData = CustomLayouts::instance().GetCustomLayoutData(layout.uuid); layoutData.has_value())
    {
        workArea->ShowLayoutNameLabel(layoutData->name);
    }
}

void FancyZones::FlashZones() noexcept
{
    if (FancyZonesSettings::settings().flashZonesOnQuickSwitch && !m_draggingState.IsDragging())
    {
        for (const auto& [_, workArea] : m_workAreaConfiguration.GetAllWorkAreas())
        {
            if (workArea)
            {
                workArea->FlashZones();
            }
        }
    }
}

void FancyZones::DockClaim(HWND window) noexcept
{
    const auto& workAreas = m_workAreaConfiguration.GetAllWorkAreas();
    for (const auto& [_, workArea] : workAreas)
    {
        if (workArea && workArea->DockTree().Find(window))
        {
            return;
        }
    }

    for (const auto& [_, workArea] : workAreas)
    {
        if (workArea && workArea->DockClaim(window))
        {
            return;
        }
    }
}

void FancyZones::DockVisibility(HWND window, bool minimizing) noexcept
{
    const bool hidden = minimizing || DockLayouts::IsHidden(window);
    for (const auto& [_, workArea] : m_workAreaConfiguration.GetAllWorkAreas())
    {
        if (workArea)
        {
            workArea->DockSetHidden(window, hidden);
        }
    }
}

void FancyZones::DockMoveByKey(DWORD vkCode) noexcept
{
    const auto window = GetForegroundWindow();
    if (!FancyZonesWindowProcessing::IsProcessableManually(window))
    {
        return;
    }

    const auto edge = vkCode == VK_LEFT ? Dock::Edge::Left : vkCode == VK_RIGHT ? Dock::Edge::Right : vkCode == VK_UP ? Dock::Edge::Top : Dock::Edge::Bottom;
    const auto& workAreas = m_workAreaConfiguration.GetAllWorkAreas();

    WorkArea* current = nullptr;
    for (const auto& [_, workArea] : workAreas)
    {
        if (workArea && workArea->DockTree().Find(window))
        {
            current = workArea.get();
        }
    }

    if (!current)
    {
        // Not docked yet: dock it on that side of its monitor
        if (const auto workArea = m_workAreaConfiguration.GetWorkAreaFromWindow(window))
        {
            if (FancyZonesWindowUtils::IsWindowMaximized(window))
            {
                ShowWindow(window, SW_RESTORE);
            }

            workArea->DockAtEdge(window, edge);
        }

        return;
    }

    if (current->DockMove(window, edge))
    {
        return;
    }

    // No neighbor on that side: go on to the monitor on that side, entering it by the edge the window comes from
    const auto toRect = [](const FancyZonesUtils::Rect& rect) { return RECT{ rect.left(), rect.top(), rect.right(), rect.bottom() }; };
    const RECT from = toRect(current->GetWorkAreaRect());
    WorkArea* next = nullptr;
    LONG nearest = 0;
    for (const auto& [_, workArea] : workAreas)
    {
        if (!workArea || workArea.get() == current)
        {
            continue;
        }

        const RECT to = toRect(workArea->GetWorkAreaRect());
        const bool sideways = edge == Dock::Edge::Left || edge == Dock::Edge::Right;
        const LONG distance = edge == Dock::Edge::Left ? from.left - to.right : edge == Dock::Edge::Right ? to.left - from.right : edge == Dock::Edge::Top ? from.top - to.bottom : to.top - from.bottom;
        const LONG overlap = sideways ? (std::min)(from.bottom, to.bottom) - (std::max)(from.top, to.top) : (std::min)(from.right, to.right) - (std::max)(from.left, to.left);
        if (distance >= 0 && overlap > 0 && (!next || distance < nearest))
        {
            next = workArea.get();
            nearest = distance;
        }
    }

    if (next)
    {
        const auto opposite = edge == Dock::Edge::Left ? Dock::Edge::Right : edge == Dock::Edge::Right ? Dock::Edge::Left : edge == Dock::Edge::Top ? Dock::Edge::Bottom : Dock::Edge::Top;
        current->UndockWindow(window);
        next->DockAtEdge(window, opposite);
    }
}

void FancyZones::DockSlot(int key, bool save) noexcept
{
    auto workArea = m_workAreaConfiguration.GetWorkAreaFromCursor();
    if (!workArea)
    {
        return;
    }

    if (save)
    {
        DockLayouts::instance().SaveSlot(key, workArea->DockTree());
        workArea->ShowLayoutNameLabel(L"Dock layout " + std::to_wstring(key) + L" saved");
        return;
    }

    auto tree = DockLayouts::instance().Slot(key);
    if (!tree)
    {
        return;
    }

    // A window is docked in one place only
    for (const auto leaf : tree->Leaves())
    {
        for (const auto& [_, other] : m_workAreaConfiguration.GetAllWorkAreas())
        {
            if (other && other.get() != workArea)
            {
                other->UndockWindow(leaf->window);
            }
        }
    }

    workArea->DockRecall(std::move(tree.value()));

    for (const auto& window : VirtualDesktop::instance().GetWindowsFromCurrentDesktop())
    {
        DockClaim(window);
    }

    workArea->ShowLayoutNameLabel(L"Dock layout " + std::to_wstring(key));
}

HMONITOR FancyZones::WorkAreaKeyFromWindow(HWND window) noexcept
{
    if (FancyZonesSettings::settings().spanZonesAcrossMonitors)
    {
        return NULL;
    }
    else
    {
        return MonitorFromWindow(window, MONITOR_DEFAULTTONULL);
    }
}

winrt::com_ptr<IFancyZones> MakeFancyZones(HINSTANCE hinstance, std::function<void()> disableCallback) noexcept
{
    return winrt::make_self<FancyZones>(hinstance, disableCallback);
}
