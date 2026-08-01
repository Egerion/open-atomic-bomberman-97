#include "bomber/ui/scheme_filename_prompt.hpp"

#include <SDL3/SDL.h>

#include <cstddef>
#include <optional>
#include <string>

#include "bomber/ui/dialog_chrome.hpp"

namespace bomber::game {

namespace {

constexpr std::size_t kMaxFilenameChars = 30;  // sub_42E938's own cap

void append_typed(std::string& entry, const char* utf8) {
    if (utf8 && entry.size() < kMaxFilenameChars) entry += utf8;
}

// What one key does to the in-progress line edit: returns the value the prompt
// closes with, or nothing while it stays open. SILENT either way — sub_42E938 is
// the generic text-entry widget and its 155-function call closure contains no
// play primitive, so the accept sting and cancel blip the port used to make here
// were both invented.
std::optional<std::string> filename_key(SDL_Keycode key, const std::string& seed,
                                        std::string& entry) {
    if (key == SDLK_BACKSPACE) {
        if (!entry.empty()) entry.pop_back();
        return std::nullopt;
    }
    if (key == SDLK_RETURN || key == SDLK_KP_ENTER)
        return entry.empty() ? seed : entry;  // "or press <Enter>" keeps the seed
    if (key == SDLK_ESCAPE) return seed;      // cancel: keep the source filename
    return std::nullopt;
}

}  // namespace

std::string SchemeFilenamePrompt::run(const std::string& seed) {
    // sub_42E938's line edit for the save-as target (sub_4028D2 exit,
    // batch_0x402150.cpp:645-648): getstring(736) "Enter schemefilename (or
    // press <Enter>):", seeded with the source filename. Enter on the seed (or
    // an empty box) keeps it; Escape cancels back to it too — the original
    // writes byte_4648C4 either way, so both return `seed`. Returns the chosen
    // stem; the caller sanitises it and appends .SCH (sub_40497C's
    // force-extension step). Interactive-only, never the demo.
    const std::string label =
        ctx_.assets.getstring(736, "Enter schemefilename (or press <Enter>):");
    std::string entry = seed;
    std::optional<std::string> result;
    SDL_StartTextInput(ctx_.window);
    while (!result) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) {
                result = seed;
                break;
            }
            if (ev.type == SDL_EVENT_TEXT_INPUT) {
                append_typed(entry, ev.text.text);
                continue;
            }
            if (ev.type != SDL_EVENT_KEY_DOWN) continue;
            if (std::optional<std::string> r = filename_key(ev.key.key, seed, entry)) result = r;
        }
        ctx_.audio.update_music();
        SDL_SetRenderDrawColor(ctx_.sdl, 0, 0, 0, 255);
        SDL_RenderClear(ctx_.sdl);
        draw_text_entry_dialog(DialogPen{ctx_.sdl, ctx_.front_font}, 180.0f,
                               TextEntryLabels{label, entry, "Done", "Cancel"});
        SDL_RenderPresent(ctx_.sdl);
        SDL_Delay(2);
    }
    SDL_StopTextInput(ctx_.window);
    return *result;
}

}  // namespace bomber::game
