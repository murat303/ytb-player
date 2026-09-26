#pragma once

#include <functional>
#include <string>

#include <borealis.hpp>

// A rounded, focusable pill with a label, like YouTube's chips and pill buttons. A tap or A
// runs the action. A light chip (dark text) marks the open section or the main action; the
// others are dark with light text.
class Chip : public brls::Box {
public:
    Chip(const std::string& text, std::function<void()> action, bool light = false);

    void setText(const std::string& text);
    void setLight(bool light);

private:
    brls::Label* label_ = nullptr;
};
