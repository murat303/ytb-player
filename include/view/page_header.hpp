#pragma once

#include <functional>
#include <string>

#include <borealis.hpp>

class SVGImage;

// The top bar of a tab, as in YouTube's tablet app: the logo with the app's name (no title) or
// the page's title on the left; search, the notifications' bell and the account on the right. The right side answers
// taps; with buttons the rail reaches the same places.
class PageHeader : public brls::Box {
public:
    PageHeader();

    void setTitle(const std::string& title);

    // Where the right-hand icons lead; the main activity sets them.
    static void setActions(std::function<void()> search, std::function<void()> notifications,
                           std::function<void()> account);

    static brls::View* create();

private:
    void showAccount();

    SVGImage* logo_ = nullptr;
    brls::Label* title_ = nullptr;
    brls::Image* avatar_ = nullptr;
    SVGImage* accountIcon_ = nullptr;
};
