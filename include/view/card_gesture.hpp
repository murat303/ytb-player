#pragma once

#include <chrono>
#include <string>

#include <borealis.hpp>

// A card's A action: plays the video, and the bottom bar says so ("Oynat"; `hint` for another
// word, as "Aç" for a playlist).
void register_play_action(brls::View* view, const brls::ActionListener& listener, const std::string& hint = {});

// A video card's touch: a tap runs its A action (play), a finger held on it for half a second
// runs its Y action (the video's page), as a long press opens more on YouTube. Moving off the
// card, or a scroll taking the touch, does neither.
class CardGesture : public brls::GestureRecognizer {
public:
    explicit CardGesture(brls::View* view);

    brls::GestureState recognitionLoop(brls::TouchState touch, brls::MouseState mouse, brls::View* view,
                                       brls::Sound* soundToPlay) override;

private:
    bool run(brls::ControllerButton button, brls::Sound* soundToPlay);
    void release();

    brls::View* view_;
    std::chrono::steady_clock::time_point pressed_{};
    bool held_ = false;       // the long press has run; the release does nothing more
    bool animating_ = false;  // the pressed look is on and has to go back
};
