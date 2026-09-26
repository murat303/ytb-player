#include "view/page_header.hpp"

#include "newpipe/auth_store.hpp"
#include "newpipe/i18n.hpp"
#include "newpipe/image_loader.hpp"
#include "view/svg_image.hpp"

namespace {

std::function<void()>& search_action() {
    static std::function<void()> action;
    return action;
}

std::function<void()>& notifications_action() {
    static std::function<void()> action;
    return action;
}

std::function<void()>& account_action() {
    static std::function<void()> action;
    return action;
}

// A tap on `view` runs the stored action (it may be set after the view is built). The view
// goes into a box that takes the tap: borealis finds touches in boxes and in views that can
// take the focus, and passes over a plain picture.
brls::Box* on_tap(brls::View* view, std::function<void()>& (*action)()) {
    auto* box = new brls::Box();
    box->setAlignItems(brls::AlignItems::CENTER);
    box->setJustifyContent(brls::JustifyContent::CENTER);
    box->addView(view);
    box->addGestureRecognizer(new brls::TapGestureRecognizer([action](brls::TapGestureStatus status, brls::Sound*) {
        if (status.state == brls::GestureState::END && action()) {
            action()();
        }
    }));
    return box;
}

}  // namespace

PageHeader::PageHeader() : brls::Box(brls::Axis::ROW) {
    this->setAlignItems(brls::AlignItems::CENTER);
    this->setHeight(46);
    this->setMarginBottom(10);

    this->logo_ = new SVGImage();
    // The app icon's disc (araclar/ikon-yap.py), not YouTube's red logo.
    this->logo_->setDimensions(26, 26);
    this->logo_->setImageFromSVGRes("svg/ytb_logo.svg");
    this->logo_->setMarginRight(8);
    this->addView(this->logo_);

    this->title_ = new brls::Label();
    this->title_->setFontSize(22);
    this->title_->setSingleLine(true);
    this->title_->setTextColor(nvgRGB(0xF1, 0xF1, 0xF1));
    this->addView(this->title_);

    auto* spacer = new brls::Box();
    spacer->setGrow(1.0f);
    this->addView(spacer);

    auto* search = new SVGImage();
    search->setDimensions(28, 28);
    search->setImageFromSVGRes("svg/search.svg");
    auto* searchBox = on_tap(search, search_action);
    searchBox->setMarginRight(22);
    this->addView(searchBox);

    auto* bell = new SVGImage();
    bell->setDimensions(28, 28);
    bell->setImageFromSVGRes("svg/notifications.svg");
    auto* bellBox = on_tap(bell, notifications_action);
    bellBox->setMarginRight(22);
    this->addView(bellBox);

    // The picked channel's picture, or a plain account icon.
    this->avatar_ = new brls::Image();
    this->avatar_->setDimensions(32, 32);
    this->avatar_->setCornerRadius(16);
    this->avatar_->setScalingType(brls::ImageScalingType::FILL);
    this->avatar_->setBackgroundColor(nvgRGB(0x3A, 0x3A, 0x3A));
    this->addView(on_tap(this->avatar_, account_action));

    this->accountIcon_ = new SVGImage();
    this->accountIcon_->setDimensions(32, 32);
    this->accountIcon_->setImageFromSVGRes("svg/account.svg");
    this->addView(on_tap(this->accountIcon_, account_action));

    this->registerStringXMLAttribute("title", [this](std::string value) { this->setTitle(value); });
    this->setTitle({});
    this->showAccount();
}

void PageHeader::setTitle(const std::string& title) {
    // Home shows the logo with the app's name; the other pages their own title.
    this->logo_->setVisibility(title.empty() ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    this->title_->setText(title.empty() ? newpipe::tr("app/title") : title);
}

void PageHeader::showAccount() {
    const auto session = newpipe::AuthStore::instance().session();
    const bool picture = session.authenticated() && !session.photo_url.empty();
    this->avatar_->setVisibility(picture ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    this->accountIcon_->setVisibility(picture ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
    if (picture) {
        newpipe::ImageLoader::instance().load(session.photo_url, this->avatar_);
    }
}

void PageHeader::setActions(std::function<void()> search, std::function<void()> notifications,
                            std::function<void()> account) {
    search_action() = std::move(search);
    notifications_action() = std::move(notifications);
    account_action() = std::move(account);
}

brls::View* PageHeader::create() {
    return new PageHeader();
}
