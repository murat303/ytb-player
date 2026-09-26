#include "view/card_gesture.hpp"

#include "newpipe/i18n.hpp"

namespace {
constexpr auto kLongPress = std::chrono::milliseconds(500);
}

void register_play_action(brls::View* view, const brls::ActionListener& listener, const std::string& hint) {
    view->registerAction(hint.empty() ? newpipe::tr("detail/play_action") : hint, brls::BUTTON_A, listener, false,
                         false, brls::SOUND_CLICK);
}

CardGesture::CardGesture(brls::View* view) : view_(view) {
}

// As brls::TapGestureRecognizer, with the time the finger has been down checked every frame.
brls::GestureState CardGesture::recognitionLoop(brls::TouchState touch, brls::MouseState mouse, brls::View* view,
                                                brls::Sound* soundToPlay) {
    brls::TouchPhase phase = touch.phase;
    brls::Point position = touch.position;
    if (phase == brls::TouchPhase::NONE) {
        position = mouse.position;
        phase = mouse.leftButton;
    }
    if (!this->enabled || phase == brls::TouchPhase::NONE) {
        return brls::GestureState::FAILED;
    }
    // A touch that failed, was taken by a scroll or already long-pressed ends here. A scroll
    // interrupts this recognizer from outside: the pressed look it started goes back then,
    // else the card stayed gray after the list moved.
    if (phase != brls::TouchPhase::START
        && (this->state == brls::GestureState::INTERRUPTED || this->state == brls::GestureState::FAILED
            || this->held_)) {
        this->release();
        return this->state;
    }

    switch (phase) {
        case brls::TouchPhase::START:
            this->state = brls::GestureState::UNSURE;
            this->pressed_ = std::chrono::steady_clock::now();
            this->held_ = false;
            brls::Application::giveFocus(this->view_);
            this->view_->playClickAnimation(false);
            this->animating_ = true;
            *soundToPlay = brls::SOUND_FOCUS_CHANGE;
            break;
        case brls::TouchPhase::STAY: {
            const bool inside = position.x >= view->getX() && position.x <= view->getX() + view->getWidth()
                && position.y >= view->getY() && position.y <= view->getY() + view->getHeight();
            if (!inside) {
                this->state = brls::GestureState::FAILED;
                this->release();
                *soundToPlay = brls::SOUND_TOUCH_UNFOCUS;
            } else if (std::chrono::steady_clock::now() - this->pressed_ >= kLongPress) {
                this->held_ = true;
                this->state = brls::GestureState::END;
                this->release();
                this->run(brls::BUTTON_Y, soundToPlay);
            }
            break;
        }
        case brls::TouchPhase::END:
            this->state = brls::GestureState::END;
            this->release();
            this->run(brls::BUTTON_A, soundToPlay);
            break;
        default:
            break;
    }
    return this->state;
}

// Undoes the pressed look once.
void CardGesture::release() {
    if (this->animating_) {
        this->animating_ = false;
        this->view_->playClickAnimation(true);
    }
}

bool CardGesture::run(brls::ControllerButton button, brls::Sound* soundToPlay) {
    for (auto& action : this->view_->getActions()) {
        if (action->getType() != brls::ACTION_GAMEPAD || action->getButton() != button || !action->isAvailable()) {
            continue;
        }
        if (action->getActionListener()(this->view_)) {
            *soundToPlay = action->getSound();
        }
        return true;
    }
    return false;
}
