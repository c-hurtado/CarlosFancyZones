#include "../pch.h"
#include "DockLayouts.h"

#include <algorithm>

#include <common/logger/logger.h>
#include <common/utils/json.h>
#include <common/utils/process_path.h>

namespace JsonUtils
{
    DWORD ProcessIdOf(HWND window) noexcept
    {
        DWORD processId = 0;
        GetWindowThreadProcessId(window, &processId);
        return processId;
    }

    json::JsonObject ToJson(const Dock::Node& node)
    {
        json::JsonObject result{};
        if (node.IsLeaf())
        {
            result.SetNamedValue(NonLocalizable::DockLayoutsIds::AppID, json::value(node.app));
            result.SetNamedValue(NonLocalizable::DockLayoutsIds::WindowID, json::value(static_cast<double>(reinterpret_cast<intptr_t>(node.window))));
            result.SetNamedValue(NonLocalizable::DockLayoutsIds::ProcessID, json::value(static_cast<double>(node.window ? ProcessIdOf(node.window) : 0)));
        }
        else
        {
            result.SetNamedValue(NonLocalizable::DockLayoutsIds::SideBySideID, json::value(node.sideBySide));
            result.SetNamedValue(NonLocalizable::DockLayoutsIds::RatioID, json::value(node.ratio));
            result.SetNamedValue(NonLocalizable::DockLayoutsIds::FirstID, ToJson(*node.first));
            result.SetNamedValue(NonLocalizable::DockLayoutsIds::SecondID, ToJson(*node.second));
        }

        return result;
    }

    // Throws winrt::hresult_error on malformed data
    std::unique_ptr<Dock::Node> NodeFromJson(const json::JsonObject& json)
    {
        auto node = std::make_unique<Dock::Node>();
        if (json.HasKey(NonLocalizable::DockLayoutsIds::FirstID))
        {
            node->sideBySide = json.GetNamedBoolean(NonLocalizable::DockLayoutsIds::SideBySideID);
            node->ratio = std::clamp(static_cast<float>(json.GetNamedNumber(NonLocalizable::DockLayoutsIds::RatioID)), 0.1f, 0.9f);
            node->first = NodeFromJson(json.GetNamedObject(NonLocalizable::DockLayoutsIds::FirstID));
            node->second = NodeFromJson(json.GetNamedObject(NonLocalizable::DockLayoutsIds::SecondID));
        }
        else
        {
            node->app = json.GetNamedString(NonLocalizable::DockLayoutsIds::AppID).c_str();

            // Handle values are reused: the handle is the saved window only when it still belongs to the same process
            const auto window = reinterpret_cast<HWND>(static_cast<intptr_t>(json.GetNamedNumber(NonLocalizable::DockLayoutsIds::WindowID)));
            const auto processId = static_cast<DWORD>(json.GetNamedNumber(NonLocalizable::DockLayoutsIds::ProcessID, 0));
            if (processId != 0 && IsWindow(window) && ProcessIdOf(window) == processId)
            {
                node->window = window;
            }
        }

        return node;
    }

    template<typename TKey, typename TKeyToString>
    json::JsonObject ToJson(const std::map<TKey, Dock::Tree>& trees, TKeyToString&& keyToString)
    {
        json::JsonObject result{};
        for (const auto& [key, tree] : trees)
        {
            if (tree.root)
            {
                result.SetNamedValue(keyToString(key), ToJson(*tree.root));
            }
        }

        return result;
    }

    // Calls add(key, tree) for every valid entry of the section. A malformed entry is skipped, the others are kept.
    template<typename TAdd>
    void ReadTrees(const json::JsonObject& data, const wchar_t* sectionId, TAdd&& add)
    {
        if (!json::has(data, sectionId))
        {
            return;
        }

        for (const auto& entry : data.GetNamedObject(sectionId))
        {
            try
            {
                Dock::Tree tree;
                tree.root = NodeFromJson(entry.Value().GetObjectW());
                add(std::wstring{ entry.Key() }, std::move(tree));
            }
            catch (const winrt::hresult_error& e)
            {
                Logger::error(L"Parsing dock-layouts error in {}: {}", sectionId, e.message());
            }
            catch (const std::exception&)
            {
                Logger::error(L"Parsing dock-layouts error in {}: invalid key", sectionId);
            }
        }
    }
}

DockLayouts::DockLayouts()
{
    const auto data = json::from_file(DockLayoutsFileName());
    if (!data)
    {
        return;
    }

    JsonUtils::ReadTrees(data.value(), NonLocalizable::DockLayoutsIds::WorkAreasID, [&](const std::wstring& key, Dock::Tree tree) {
        Refresh(tree);
        m_trees[key] = std::move(tree);
    });

    JsonUtils::ReadTrees(data.value(), NonLocalizable::DockLayoutsIds::SlotsID, [&](const std::wstring& key, Dock::Tree tree) {
        size_t parsed = 0;
        const int slot = std::stoi(key, &parsed);
        if (parsed != key.size())
        {
            throw std::invalid_argument("slot key");
        }

        m_slots[slot] = std::move(tree);
    });
}

DockLayouts& DockLayouts::instance()
{
    static DockLayouts self;
    return self;
}

bool DockLayouts::IsHidden(HWND window) noexcept
{
    // A window on another virtual desktop is cloaked by the shell
    int cloaked = 0;
    DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    return IsIconic(window) || !IsWindowVisible(window) || (cloaked & DWM_CLOAKED_SHELL) != 0;
}

void DockLayouts::Refresh(Dock::Tree& tree)
{
    for (auto leaf : tree.Leaves())
    {
        leaf->placedAt = 0;
        if (!leaf->window)
        {
            continue;
        }

        if (!IsWindow(leaf->window) || GetAncestor(leaf->window, GA_ROOT) != leaf->window || get_process_path(leaf->window) != leaf->app)
        {
            leaf->window = nullptr;
            leaf->hidden = false;
        }
        else
        {
            leaf->hidden = IsHidden(leaf->window);
        }
    }
}

Dock::Tree& DockLayouts::Tree(const FancyZonesDataTypes::WorkAreaId& id)
{
    return m_trees[id.toString()];
}

bool DockLayouts::RemoveWindow(HWND window)
{
    bool removed = false;
    for (auto& [_, tree] : m_trees)
    {
        removed |= tree.Remove(window);
    }

    if (removed)
    {
        SaveData();
    }

    return removed;
}

bool DockLayouts::HasSlot(int key) const noexcept
{
    return m_slots.contains(key);
}

void DockLayouts::SaveSlot(int key, const Dock::Tree& tree)
{
    if (tree.root)
    {
        m_slots[key] = tree.Clone();
    }
    else
    {
        m_slots.erase(key);
    }

    SaveData();
}

std::optional<Dock::Tree> DockLayouts::Slot(int key) const
{
    auto iter = m_slots.find(key);
    if (iter == m_slots.end())
    {
        return std::nullopt;
    }

    auto tree = iter->second.Clone();
    Refresh(tree);
    return tree;
}

void DockLayouts::SaveData() const
{
    json::JsonObject root{};
    root.SetNamedValue(NonLocalizable::DockLayoutsIds::WorkAreasID, JsonUtils::ToJson(m_trees, [](const std::wstring& id) { return id; }));
    root.SetNamedValue(NonLocalizable::DockLayoutsIds::SlotsID, JsonUtils::ToJson(m_slots, [](int key) { return std::to_wstring(key); }));
    json::to_file(DockLayoutsFileName(), root);
}
