#include "pch.h"
#include "FancyZonesLib\DockTree.h"

#include "Util.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace FancyZonesUnitTests
{
    TEST_CLASS (DockTreeUnitTests)
    {
        const RECT m_area{ 0, 0, 1000, 800 };
        const HWND m_a = reinterpret_cast<HWND>(1);
        const HWND m_b = reinterpret_cast<HWND>(2);
        const HWND m_c = reinterpret_cast<HWND>(3);

        void DockAt(Dock::Tree& tree, HWND window, POINT pt)
        {
            const auto target = tree.TargetFromPoint(m_area, pt, 0);
            Assert::IsTrue(target.has_value());
            tree.Insert(window, L"", target.value());
        }

        // An invalid rect when the window has no space in the layout
        RECT RectOf(const Dock::Tree& tree, HWND window, int gap = 0)
        {
            RECT result{ -1, -1, -1, -1 };
            for (const auto& [rectWindow, rect] : tree.WindowRects(m_area, gap))
            {
                if (rectWindow == window)
                {
                    result = rect;
                }
            }

            return result;
        }

        // a on the left half, b on the right half
        Dock::Tree SideBySide()
        {
            Dock::Tree tree;
            DockAt(tree, m_a, { 10, 400 });
            DockAt(tree, m_b, { 750, 400 });
            return tree;
        }

    public:
        TEST_METHOD (FirstWindowTakesHalfOfWorkArea)
        {
            Dock::Tree tree;
            const auto target = tree.TargetFromPoint(m_area, { 10, 400 }, 0);
            Assert::IsTrue(target.has_value());
            CustomAssert::AreEqual(RECT{ 0, 0, 500, 800 }, target->preview);

            tree.Insert(m_a, L"", target.value());
            CustomAssert::AreEqual(RECT{ 0, 0, 500, 800 }, RectOf(tree, m_a));
        }

        TEST_METHOD (NoTargetAwayFromEdgesOfEmptyWorkArea)
        {
            Dock::Tree tree;
            Assert::IsFalse(tree.TargetFromPoint(m_area, { 500, 400 }, 0).has_value());
        }

        TEST_METHOD (SecondWindowFillsEmptySlot)
        {
            Dock::Tree tree;
            DockAt(tree, m_a, { 10, 400 });

            const auto target = tree.TargetFromPoint(m_area, { 750, 400 }, 0);
            Assert::IsTrue(target.has_value());
            CustomAssert::AreEqual(RECT{ 500, 0, 1000, 800 }, target->preview);

            tree.Insert(m_b, L"", target.value());
            CustomAssert::AreEqual(RECT{ 0, 0, 500, 800 }, RectOf(tree, m_a));
            CustomAssert::AreEqual(RECT{ 500, 0, 1000, 800 }, RectOf(tree, m_b));
        }

        TEST_METHOD (DropOnWindowEdgeSplitsWindow)
        {
            auto tree = SideBySide();
            DockAt(tree, m_c, { 750, 700 });

            CustomAssert::AreEqual(RECT{ 0, 0, 500, 800 }, RectOf(tree, m_a));
            CustomAssert::AreEqual(RECT{ 500, 0, 1000, 400 }, RectOf(tree, m_b));
            CustomAssert::AreEqual(RECT{ 500, 400, 1000, 800 }, RectOf(tree, m_c));
        }

        TEST_METHOD (DropOnWorkAreaEdgeSplitsWorkArea)
        {
            auto tree = SideBySide();
            DockAt(tree, m_c, { 500, 790 });

            CustomAssert::AreEqual(RECT{ 0, 0, 500, 400 }, RectOf(tree, m_a));
            CustomAssert::AreEqual(RECT{ 500, 0, 1000, 400 }, RectOf(tree, m_b));
            CustomAssert::AreEqual(RECT{ 0, 400, 1000, 800 }, RectOf(tree, m_c));
        }

        TEST_METHOD (NoTargetInMiddleOfWindow)
        {
            auto tree = SideBySide();
            Assert::IsFalse(tree.TargetFromPoint(m_area, { 750, 400 }, 0).has_value());
        }

        TEST_METHOD (RemovedWindowGivesSpaceToNeighbor)
        {
            auto tree = SideBySide();
            DockAt(tree, m_c, { 750, 700 });

            Assert::IsTrue(tree.Remove(m_c));
            CustomAssert::AreEqual(RECT{ 500, 0, 1000, 800 }, RectOf(tree, m_b));

            Assert::IsTrue(tree.Remove(m_b));
            CustomAssert::AreEqual(m_area, RectOf(tree, m_a));

            Assert::IsFalse(tree.Remove(m_b));
        }

        TEST_METHOD (RemovingOnlyWindowEmptiesTree)
        {
            Dock::Tree tree;
            DockAt(tree, m_a, { 10, 400 });

            Assert::IsTrue(tree.Remove(m_a));
            Assert::IsTrue(tree.root == nullptr);
        }

        TEST_METHOD (HiddenWindowGivesSpaceAndTakesItBack)
        {
            auto tree = SideBySide();

            tree.Find(m_b)->hidden = true;
            Assert::AreEqual(size_t{ 1 }, tree.WindowRects(m_area, 0).size());
            CustomAssert::AreEqual(m_area, RectOf(tree, m_a));

            tree.Find(m_b)->hidden = false;
            CustomAssert::AreEqual(RECT{ 0, 0, 500, 800 }, RectOf(tree, m_a));
            CustomAssert::AreEqual(RECT{ 500, 0, 1000, 800 }, RectOf(tree, m_b));
        }

        TEST_METHOD (ResizedWindowResizesNeighbor)
        {
            auto tree = SideBySide();

            Assert::IsTrue(tree.Resize(m_a, m_area, 0, RECT{ 0, 0, 500, 800 }, RECT{ 0, 0, 300, 800 }));
            CustomAssert::AreEqual(RECT{ 0, 0, 300, 800 }, RectOf(tree, m_a));
            CustomAssert::AreEqual(RECT{ 300, 0, 1000, 800 }, RectOf(tree, m_b));
        }

        TEST_METHOD (ResizeOnSideWithoutNeighborChangesNothing)
        {
            auto tree = SideBySide();

            Assert::IsFalse(tree.Resize(m_a, m_area, 0, RECT{ 0, 0, 500, 800 }, RECT{ 0, 0, 500, 600 }));
            CustomAssert::AreEqual(RECT{ 0, 0, 500, 800 }, RectOf(tree, m_a));
        }

        TEST_METHOD (ResizeIgnoresEdgesTheUserDidNotMove)
        {
            auto tree = SideBySide();

            // The window was already wider than its place, only its bottom edge was dragged
            Assert::IsFalse(tree.Resize(m_a, m_area, 0, RECT{ 0, 0, 600, 800 }, RECT{ 0, 0, 600, 600 }));
            CustomAssert::AreEqual(RECT{ 500, 0, 1000, 800 }, RectOf(tree, m_b));
        }

        TEST_METHOD (ResizeMovesSplitFromWhereTheWindowWas)
        {
            auto tree = SideBySide();

            Assert::IsTrue(tree.Resize(m_a, m_area, 0, RECT{ 0, 0, 600, 800 }, RECT{ 0, 0, 400, 800 }));
            CustomAssert::AreEqual(RECT{ 400, 0, 1000, 800 }, RectOf(tree, m_b));
        }

        TEST_METHOD (MoveSwapsWithNeighbor)
        {
            auto tree = SideBySide();

            Assert::IsTrue(tree.Move(m_a, m_area, 0, Dock::Edge::Right));
            CustomAssert::AreEqual(RECT{ 500, 0, 1000, 800 }, RectOf(tree, m_a));
            CustomAssert::AreEqual(RECT{ 0, 0, 500, 800 }, RectOf(tree, m_b));
        }

        TEST_METHOD (MoveWithoutNeighborOnThatSideChangesNothing)
        {
            auto tree = SideBySide();

            Assert::IsFalse(tree.Move(m_a, m_area, 0, Dock::Edge::Left));
            Assert::IsFalse(tree.Move(m_a, m_area, 0, Dock::Edge::Top));
            CustomAssert::AreEqual(RECT{ 0, 0, 500, 800 }, RectOf(tree, m_a));
        }

        TEST_METHOD (MoveTakesEmptySlot)
        {
            Dock::Tree tree;
            DockAt(tree, m_a, { 10, 400 });

            Assert::IsTrue(tree.Move(m_a, m_area, 0, Dock::Edge::Right));
            CustomAssert::AreEqual(RECT{ 500, 0, 1000, 800 }, RectOf(tree, m_a));
        }

        TEST_METHOD (MovePicksNeighborOnThatSide)
        {
            // a on the left, b above c on the right
            auto tree = SideBySide();
            DockAt(tree, m_c, { 750, 700 });

            Assert::IsTrue(tree.Move(m_c, m_area, 0, Dock::Edge::Top));
            CustomAssert::AreEqual(RECT{ 500, 0, 1000, 400 }, RectOf(tree, m_c));
            CustomAssert::AreEqual(RECT{ 500, 400, 1000, 800 }, RectOf(tree, m_b));

            Assert::IsTrue(tree.Move(m_c, m_area, 0, Dock::Edge::Left));
            CustomAssert::AreEqual(RECT{ 0, 0, 500, 800 }, RectOf(tree, m_c));
            CustomAssert::AreEqual(RECT{ 500, 0, 1000, 400 }, RectOf(tree, m_a));
        }

        TEST_METHOD (GapIsLeftBetweenWindows)
        {
            const auto tree = SideBySide();

            CustomAssert::AreEqual(RECT{ 0, 0, 495, 800 }, RectOf(tree, m_a, 10));
            CustomAssert::AreEqual(RECT{ 505, 0, 1000, 800 }, RectOf(tree, m_b, 10));
        }

        TEST_METHOD (CloneIsIndependent)
        {
            auto tree = SideBySide();
            auto copy = tree.Clone();

            tree.Remove(m_b);
            CustomAssert::AreEqual(RECT{ 500, 0, 1000, 800 }, RectOf(copy, m_b));
        }
    };
}
