#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

// Dock layout of one work area: a binary tree of splits whose leaves are windows.
// Rects are in work area coordinates.
namespace Dock
{
    enum class Edge
    {
        Left,
        Top,
        Right,
        Bottom
    };

    struct Node
    {
        // Leaf
        HWND window{}; // nullptr: an empty slot
        std::wstring app; // Process path. On an empty slot: the app the slot is kept for
        bool hidden{}; // Minimized or hidden window: keeps its place in the tree, but takes no space
        ULONGLONG placedAt{}; // Tick count of the first time the window was moved to its place, 0 until then

        // Split
        bool sideBySide{}; // true: first is on the left of second, false: first is above second
        float ratio{ 0.5f }; // Share of the split taken by first
        std::unique_ptr<Node> first;
        std::unique_ptr<Node> second;

        bool IsLeaf() const noexcept { return !first; }
    };

    // Where a dragged window lands
    struct Target
    {
        Node* node{}; // nullptr: the whole work area
        Edge edge{};
        bool fill{}; // node is an empty slot taken over by the window
        RECT preview{};
    };

    class Tree
    {
    public:
        std::unique_ptr<Node> root;

        Tree Clone() const;
        std::vector<Node*> Leaves() const;
        Node* Find(HWND window) const;

        std::optional<Target> TargetFromPoint(const RECT& area, POINT pt, int gap);
        void Insert(HWND window, const std::wstring& app, const Target& target);
        // Swaps the window with the leaf next to it on that side. False when there is none.
        bool Move(HWND window, const RECT& area, int gap, Edge direction);
        // The freed space goes to the neighbors
        bool Remove(HWND window);
        // Moves the splits bounding the window by the edges the user moved, from the rect the window had to the new one
        bool Resize(HWND window, const RECT& area, int gap, const RECT& fromRect, const RECT& toRect);

        std::vector<std::pair<HWND, RECT>> WindowRects(const RECT& area, int gap) const;
    };
}
