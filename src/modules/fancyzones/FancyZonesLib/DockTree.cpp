#include "pch.h"
#include "DockTree.h"

#include <algorithm>
#include <array>
#include <cmath>

using namespace Dock;

namespace
{
    constexpr float MinRatio = 0.1f;

    // A window isn't docked where it would get less than this, in either direction
    constexpr LONG MinTargetSize = 100;

    bool IsEmptySlot(const Node& node) noexcept
    {
        return node.IsLeaf() && !node.window;
    }

    bool Occupies(const Node& node) noexcept
    {
        return node.IsLeaf() ? (node.window && !node.hidden) : (Occupies(*node.first) || Occupies(*node.second));
    }

    bool HasEmptySlot(const Node& node) noexcept
    {
        return node.IsLeaf() ? !node.window : (HasEmptySlot(*node.first) || HasEmptySlot(*node.second));
    }

    bool OnlyEmptySlots(const Node& node) noexcept
    {
        return node.IsLeaf() ? !node.window : (OnlyEmptySlots(*node.first) && OnlyEmptySlots(*node.second));
    }

    // Empty slots keep their share as long as the other side of the split is in use.
    // slotsOnly: no window takes space here, the empty slots are laid out instead so that they can be filled.
    bool TakesSpace(const Node& node, const Node& sibling, bool slotsOnly = false) noexcept
    {
        return slotsOnly ? HasEmptySlot(node) : (Occupies(node) || (OnlyEmptySlots(node) && Occupies(sibling)));
    }

    constexpr bool IsSideEdge(Edge edge) noexcept
    {
        return edge == Edge::Left || edge == Edge::Right;
    }

    constexpr bool IsLeadingEdge(Edge edge) noexcept
    {
        return edge == Edge::Left || edge == Edge::Top;
    }

    std::pair<RECT, RECT> Split(const RECT& rect, bool sideBySide, float ratio, int gap) noexcept
    {
        RECT first = rect;
        RECT second = rect;
        if (sideBySide)
        {
            const LONG cut = rect.left + std::lround((rect.right - rect.left) * ratio);
            first.right = cut - gap / 2;
            second.left = cut + (gap - gap / 2);
        }
        else
        {
            const LONG cut = rect.top + std::lround((rect.bottom - rect.top) * ratio);
            first.bottom = cut - gap / 2;
            second.top = cut + (gap - gap / 2);
        }

        return { first, second };
    }

    RECT HalfAt(const RECT& rect, Edge edge) noexcept
    {
        const auto [first, second] = Split(rect, IsSideEdge(edge), 0.5f, 0);
        return IsLeadingEdge(edge) ? first : second;
    }

    // Calls fn(node, rect, depth) for every node that gets space, parents first
    template<typename F>
    void Visit(Node& node, const RECT& rect, int gap, F&& fn, size_t depth = 0, bool slotsOnly = false)
    {
        fn(node, rect, depth);
        if (node.IsLeaf())
        {
            return;
        }

        // Slots kept next to a window in use share the space they were given
        slotsOnly = slotsOnly || !Occupies(node);

        const bool first = TakesSpace(*node.first, *node.second, slotsOnly);
        const bool second = TakesSpace(*node.second, *node.first, slotsOnly);
        if (first && second)
        {
            const auto [firstRect, secondRect] = Split(rect, node.sideBySide, node.ratio, gap);
            Visit(*node.first, firstRect, gap, fn, depth + 1, slotsOnly);
            Visit(*node.second, secondRect, gap, fn, depth + 1, slotsOnly);
        }
        else if (first)
        {
            Visit(*node.first, rect, gap, fn, depth + 1, slotsOnly);
        }
        else if (second)
        {
            Visit(*node.second, rect, gap, fn, depth + 1, slotsOnly);
        }
    }

    void CollectLeaves(Node* node, std::vector<Node*>& leaves)
    {
        if (!node)
        {
            return;
        }

        if (node->IsLeaf())
        {
            leaves.push_back(node);
            return;
        }

        CollectLeaves(node->first.get(), leaves);
        CollectLeaves(node->second.get(), leaves);
    }

    std::unique_ptr<Node> CloneNode(const Node* node)
    {
        if (!node)
        {
            return nullptr;
        }

        auto copy = std::make_unique<Node>();
        copy->window = node->window;
        copy->app = node->app;
        copy->hidden = node->hidden;
        copy->placedAt = node->placedAt;
        copy->sideBySide = node->sideBySide;
        copy->ratio = node->ratio;
        copy->first = CloneNode(node->first.get());
        copy->second = CloneNode(node->second.get());
        return copy;
    }

    std::unique_ptr<Node>* OwnerOf(std::unique_ptr<Node>* slot, const Node* node)
    {
        if (!*slot || slot->get() == node)
        {
            return *slot ? slot : nullptr;
        }

        if ((*slot)->IsLeaf())
        {
            return nullptr;
        }

        auto owner = OwnerOf(&(*slot)->first, node);
        return owner ? owner : OwnerOf(&(*slot)->second, node);
    }

    bool RemoveFrom(std::unique_ptr<Node>& slot, HWND window)
    {
        if (slot->IsLeaf())
        {
            if (slot->window != window)
            {
                return false;
            }

            slot.reset();
            return true;
        }

        if (!RemoveFrom(slot->first, window) && !RemoveFrom(slot->second, window))
        {
            return false;
        }

        if (!slot->first || !slot->second)
        {
            // The remaining side takes over the split
            slot = std::move(slot->first ? slot->first : slot->second);
        }
        else if (IsEmptySlot(*slot->first) && IsEmptySlot(*slot->second) && (slot->first->app.empty() || slot->second->app.empty()))
        {
            // Two empty slots become one. A slot kept for an app stays.
            slot = std::move(slot->first->app.empty() ? slot->second : slot->first);
        }

        return true;
    }
}

Tree Tree::Clone() const
{
    Tree copy;
    copy.root = CloneNode(root.get());
    return copy;
}

std::vector<Node*> Tree::Leaves() const
{
    std::vector<Node*> leaves;
    CollectLeaves(root.get(), leaves);
    return leaves;
}

Node* Tree::Find(HWND window) const
{
    if (!window)
    {
        return nullptr;
    }

    for (auto leaf : Leaves())
    {
        if (leaf->window == window)
        {
            return leaf;
        }
    }

    return nullptr;
}

std::optional<Target> Tree::TargetFromPoint(const RECT& area, POINT pt, int gap, LONG edgeBand, float windowEdgeShare)
{
    if (!PtInRect(&area, pt))
    {
        return std::nullopt;
    }

    const std::array<std::pair<LONG, Edge>, 4> areaEdges = { {
        { pt.x - area.left, Edge::Left },
        { pt.y - area.top, Edge::Top },
        { area.right - pt.x, Edge::Right },
        { area.bottom - pt.y, Edge::Bottom },
    } };
    const auto nearestAreaEdge = *std::min_element(areaEdges.begin(), areaEdges.end());

    // No window takes space: the slots kept for apps are the targets
    const bool slotsOnly = root && !Occupies(*root);

    std::optional<Target> target;
    bool found = nearestAreaEdge.first < edgeBand;
    if (found)
    {
        target = Target{ .node = nullptr, .edge = nearestAreaEdge.second, .fill = false, .preview = HalfAt(area, nearestAreaEdge.second) };
    }

    const auto hitTest = [&](Node& node, const RECT& rect, size_t) {
        if (found || !node.IsLeaf())
        {
            return;
        }

        // The root is visited even when it is a hidden window, which takes no space
        if (node.window && node.hidden)
        {
            return;
        }

        // The gap around a leaf belongs to it
        RECT hitRect = rect;
        InflateRect(&hitRect, gap, gap);
        if (!PtInRect(&hitRect, pt))
        {
            return;
        }

        found = true;
        if (!node.window)
        {
            target = Target{ .node = &node, .edge = Edge::Left, .fill = true, .preview = rect };
            return;
        }

        const float width = static_cast<float>(rect.right - rect.left);
        const float height = static_cast<float>(rect.bottom - rect.top);
        if (width <= 0 || height <= 0)
        {
            return;
        }

        const std::array<std::pair<float, Edge>, 4> edges = { {
            { (pt.x - rect.left) / width, Edge::Left },
            { (pt.y - rect.top) / height, Edge::Top },
            { (rect.right - pt.x) / width, Edge::Right },
            { (rect.bottom - pt.y) / height, Edge::Bottom },
        } };
        const auto nearestEdge = *std::min_element(edges.begin(), edges.end());
        if (nearestEdge.first <= windowEdgeShare)
        {
            target = Target{ .node = &node, .edge = nearestEdge.second, .fill = false, .preview = HalfAt(rect, nearestEdge.second) };
        }
    };

    if (root && !found)
    {
        Visit(*root, area, gap, hitTest, 0, slotsOnly);
    }

    if (target && slotsOnly)
    {
        // The window will be the only one taking space, which can be more than the slot or half under the cursor
        auto copy = Clone();
        Target copyTarget = target.value();
        if (target->node)
        {
            const auto leaves = Leaves();
            copyTarget.node = copy.Leaves()[std::find(leaves.begin(), leaves.end(), target->node) - leaves.begin()];
        }

        const auto placeholder = reinterpret_cast<HWND>(static_cast<intptr_t>(-1));
        copy.Insert(placeholder, L"", copyTarget);
        for (const auto& [window, rect] : copy.WindowRects(area, gap))
        {
            if (window == placeholder)
            {
                target->preview = rect;
            }
        }
    }

    if (target && (target->preview.right - target->preview.left < MinTargetSize || target->preview.bottom - target->preview.top < MinTargetSize))
    {
        return std::nullopt;
    }

    return target;
}

void Tree::Insert(HWND window, const std::wstring& app, const Target& target)
{
    if (target.fill && target.node)
    {
        target.node->window = window;
        target.node->app = app;
        target.node->hidden = false;
        target.node->placedAt = 0;
        return;
    }

    auto owner = target.node ? OwnerOf(&root, target.node) : &root;
    if (!owner)
    {
        return;
    }

    auto leaf = std::make_unique<Node>();
    leaf->window = window;
    leaf->app = app;

    // The first window docked on a work area gets half of it, the other half stays an empty slot
    auto other = *owner ? std::move(*owner) : std::make_unique<Node>();

    auto split = std::make_unique<Node>();
    split->sideBySide = IsSideEdge(target.edge);
    if (IsLeadingEdge(target.edge))
    {
        split->first = std::move(leaf);
        split->second = std::move(other);
    }
    else
    {
        split->first = std::move(other);
        split->second = std::move(leaf);
    }

    *owner = std::move(split);
}

bool Tree::Move(HWND window, const RECT& area, int gap, Edge direction)
{
    if (!window || !root || !Occupies(*root))
    {
        return false;
    }

    std::vector<std::pair<Node*, RECT>> leaves;
    const RECT* from = nullptr;
    Node* fromLeaf = nullptr;
    Visit(*root, area, gap, [&](Node& node, const RECT& rect, size_t) {
        if (node.IsLeaf())
        {
            leaves.emplace_back(&node, rect);
        }
    });

    for (const auto& [leaf, rect] : leaves)
    {
        if (leaf->window == window)
        {
            fromLeaf = leaf;
            from = &rect;
        }
    }

    if (!fromLeaf)
    {
        return false;
    }

    // The nearest leaf on that side, and of those the one sharing the longest border with the window
    Node* toLeaf = nullptr;
    std::pair<LONG, LONG> best{};
    for (const auto& [leaf, rect] : leaves)
    {
        LONG distance = 0;
        LONG overlap = 0;
        if (IsSideEdge(direction))
        {
            distance = direction == Edge::Left ? from->left - rect.right : rect.left - from->right;
            overlap = (std::min)(from->bottom, rect.bottom) - (std::max)(from->top, rect.top);
        }
        else
        {
            distance = direction == Edge::Top ? from->top - rect.bottom : rect.top - from->bottom;
            overlap = (std::min)(from->right, rect.right) - (std::max)(from->left, rect.left);
        }

        if (leaf == fromLeaf || distance < 0 || overlap <= 0)
        {
            continue;
        }

        const std::pair<LONG, LONG> candidate{ distance, -overlap };
        if (!toLeaf || candidate < best)
        {
            toLeaf = leaf;
            best = candidate;
        }
    }

    if (!toLeaf)
    {
        return false;
    }

    std::swap(fromLeaf->window, toLeaf->window);
    std::swap(fromLeaf->app, toLeaf->app);
    std::swap(fromLeaf->hidden, toLeaf->hidden);
    fromLeaf->placedAt = 0;
    toLeaf->placedAt = 0;
    return true;
}

bool Tree::Remove(HWND window)
{
    if (!window || !root || !RemoveFrom(root, window))
    {
        return false;
    }

    if (root && IsEmptySlot(*root) && root->app.empty())
    {
        root.reset();
    }

    return true;
}

bool Tree::Resize(HWND window, const RECT& area, int gap, const RECT& fromRect, const RECT& toRect)
{
    if (!window || !root || !Occupies(*root))
    {
        return false;
    }

    // Nodes from the root down to the window, with their rects
    std::vector<std::pair<Node*, RECT>> path;
    std::vector<std::pair<Node*, RECT>> current;
    Visit(*root, area, gap, [&](Node& node, const RECT& rect, size_t depth) {
        current.resize(depth);
        current.emplace_back(&node, rect);
        if (node.IsLeaf() && node.window == window)
        {
            path = current;
        }
    });

    if (path.empty())
    {
        return false;
    }

    bool changed = false;

    // The edge is defined by the nearest split of that direction which has the window on the other side of the cut
    const auto moveEdge = [&](LONG oldEdge, LONG newEdge, bool sideBySide, bool windowInFirst) {
        if (std::abs(newEdge - oldEdge) < 2)
        {
            return;
        }

        for (size_t i = path.size() - 1; i-- > 0;)
        {
            Node& split = *path[i].first;
            const bool inFirst = split.first.get() == path[i + 1].first;
            if (split.sideBySide != sideBySide || inFirst != windowInFirst)
            {
                continue;
            }

            if (!TakesSpace(*split.first, *split.second) || !TakesSpace(*split.second, *split.first))
            {
                continue;
            }

            const RECT& rect = path[i].second;
            const LONG start = sideBySide ? rect.left : rect.top;
            const LONG extent = sideBySide ? rect.right - rect.left : rect.bottom - rect.top;
            if (extent <= 0)
            {
                return;
            }

            const LONG cut = windowInFirst ? newEdge + gap / 2 : newEdge - (gap - gap / 2);
            split.ratio = std::clamp(static_cast<float>(cut - start) / extent, MinRatio, 1.0f - MinRatio);
            changed = true;
            return;
        }
    };

    moveEdge(fromRect.left, toRect.left, true, false);
    moveEdge(fromRect.right, toRect.right, true, true);
    moveEdge(fromRect.top, toRect.top, false, false);
    moveEdge(fromRect.bottom, toRect.bottom, false, true);

    return changed;
}

std::vector<std::pair<HWND, RECT>> Tree::WindowRects(const RECT& area, int gap) const
{
    std::vector<std::pair<HWND, RECT>> rects;
    if (root && Occupies(*root))
    {
        Visit(*root, area, gap, [&](Node& node, const RECT& rect, size_t) {
            if (node.IsLeaf() && node.window)
            {
                rects.emplace_back(node.window, rect);
            }
        });
    }

    return rects;
}
