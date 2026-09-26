#include "view/chip.hpp"

Chip::Chip(const std::string& text, std::function<void()> action, bool light) : brls::Box(brls::Axis::ROW) {
    this->setFocusable(true);
    this->setCornerRadius(18);
    this->setHighlightCornerRadius(18);
    // The focus highlight's own background darkened a light chip and hid its text.
    this->setHideHighlightBackground(true);
    this->setPadding(8, 18, 8, 18);
    this->setMarginRight(10);
    this->setAlignItems(brls::AlignItems::CENTER);

    this->label_ = new brls::Label();
    this->label_->setFontSize(17);
    this->label_->setSingleLine(true);
    this->label_->setText(text);
    this->addView(this->label_);

    this->registerClickAction([action](brls::View*) {
        action();
        return true;
    });
    this->addGestureRecognizer(new brls::TapGestureRecognizer(this));
    this->setLight(light);
}

void Chip::setText(const std::string& text) {
    this->label_->setText(text);
}

void Chip::setLight(bool light) {
    this->setBackgroundColor(light ? nvgRGB(0xF1, 0xF1, 0xF1) : nvgRGB(0x3A, 0x3A, 0x3A));
    this->label_->setTextColor(light ? nvgRGB(0x0F, 0x0F, 0x0F) : nvgRGB(0xF1, 0xF1, 0xF1));
}
