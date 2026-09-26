#pragma once

#include <borealis.hpp>

#include <functional>
#include <string>

#include "newpipe/i18n.hpp"
#include "view/auto_tab_frame.hpp"

namespace newpipe {

// True while the focus is somewhere inside `root` (a page), false on the sidebar.
inline bool focus_inside(brls::View* root) {
    for (brls::View* view = brls::Application::getCurrentFocus(); view; view = view->getParent()) {
        if (view == root) {
            return true;
        }
    }
    return false;
}

// L and R step through the chips (tabs) at the top of a page: step(-1) and step(+1). One
// hint stands for both: the R glyph leads its text and the hint bar draws it beside the L one.
inline std::string tab_step_hint() {
    return std::string(" ") + tr("hints/tabs");
}

inline void register_tab_step(brls::View* view, const std::function<void(int)>& step) {
    view->registerAction(tab_step_hint(), brls::ControllerButton::BUTTON_LB, [step](brls::View*) {
        step(-1);
        return true;
    });
    view->registerAction(tab_step_hint(), brls::ControllerButton::BUTTON_RB, [step](brls::View*) {
        step(1);
        return true;
    }, true);
}

// The same for a sidebar tab: mirrored on its sidebar item, as the other tab actions.
inline void register_tab_step(AttachedView* tab, const std::function<void(int)>& step) {
    tab->registerTabAction(tab_step_hint(), brls::ControllerButton::BUTTON_LB, [step](brls::View*) {
        step(-1);
        return true;
    });
    tab->registerTabAction(tab_step_hint(), brls::ControllerButton::BUTTON_RB, [step](brls::View*) {
        step(1);
        return true;
    }, true);
}

// L/R moved to another tab from inside the page: back to the top, where the chips are, and the
// focus on the new chip, so that down leads to the new list's first video. Scrolling first
// keeps the chip in view; a NATURAL frame takes the focus itself from a view outside it.
inline void focus_tab_chip(brls::View* chip, brls::ScrollingFrame* frame) {
    if (frame) {
        frame->setContentOffsetY(0, false);
    }
    brls::Application::giveFocus(chip);
}

// Rows of Shorts are taller than half the screen. A NATURAL frame moves the focus to the next
// row only once all of it is in view and a quick press scrolls less than that, so the focus
// ended on the frame itself, where nothing shows it. Such rows are centred instead.
inline void set_grid_scrolling(brls::ScrollingFrame* frame, bool tall_rows) {
    if (frame) {
        frame->setScrollingBehavior(tall_rows ? brls::ScrollingBehavior::CENTERED : brls::ScrollingBehavior::NATURAL);
    }
}

// Call this before clearing a card grid.
//
// borealis clears Application::currentFocus when the focused view is destroyed
// (View::~View focus sanity check), and Application::navigate() bails out while
// there is no focus, so deleting the focused card leaves the whole UI unable to
// move. Handing focus to the scrolling frame does not help: its default focus
// resolves through lastFocusedView back to the very card that is about to be
// deleted, which makes giveFocus() a no-op.
//
// The sidebar item is always focusable and never owned by the grid, so it is a
// safe place to park focus. Giving it focus does not switch tabs: the item is
// already the active one, and AutoSidebarItem::setActive() ignores no-op
// changes.
inline void release_grid_focus(AttachedView* tab, brls::Box* gridBox) {
    if (!gridBox) {
        return;
    }

    for (brls::View* view = brls::Application::getCurrentFocus(); view; view = view->getParent()) {
        if (view != gridBox) {
            continue;
        }

        brls::View* sidebarItem = tab ? tab->getTabBar() : nullptr;
        if (sidebarItem) {
            brls::Application::giveFocus(sidebarItem);
        }
        return;
    }
}

}  // namespace newpipe
