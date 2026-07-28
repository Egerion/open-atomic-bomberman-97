#include "bomber/game/screens/scheme_filename_prompt.hpp"

#include <SDL3/SDL.h>

#include "bomber/game/dialog_chrome.hpp"

namespace bomber::game {

std::string SchemeFilenamePrompt::run(const std::string& seed) {
    // sub_42E938 line-edit for the save-as target (sub_4028D2 exit,
    // batch_0x402150.cpp:645-648): getstring(736) "Enter schemefilename (or
    // press <Enter>):", max 30 chars, seeded with the source filename. Enter on
    // the seed (or an empty box) keeps the seed; Escape cancels back to the seed
    // too — the original writes byte_4648C4 either way, so both return `seed`.
    // Returns the chosen stem (no extension; the caller sanitises + appends .SCH,
    // the sub_40497C force-extension step). Interactive-only (never the demo).
    const std::string label = ctx_.assets.getstring(736, "Enter schemefilename (or press <Enter>):");
    std::string entry = seed;
    std::string result = seed;
    SDL_StartTextInput(ctx_.window);
    bool waiting = true;
    while (waiting) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) {
                waiting = false;  // result stays `seed`
                break;
            }
            if (ev.type == SDL_EVENT_TEXT_INPUT) {
                if (ev.text.text && entry.size() < 30) entry += ev.text.text;  // 30-char cap
            } else if (ev.type == SDL_EVENT_KEY_DOWN) {
                if (ev.key.key == SDLK_BACKSPACE) {
                    if (!entry.empty()) entry.pop_back();
                } else if (ev.key.key == SDLK_RETURN || ev.key.key == SDLK_KP_ENTER) {
                    // SILENT, both ways. sub_42E938 is the generic text-entry
                    // widget and it makes no sound at all — its call closure
                    // (155 functions) contains no play primitive. The accept
                    // sting and the cancel blip here were both invented.
                    result = entry.empty() ? seed : entry;  // "or press <Enter>" keeps the seed
                    waiting = false;
                } else if (ev.key.key == SDLK_ESCAPE) {
                    result = seed;  // cancel: keep the source filename
                    waiting = false;
                }
            }
        }
        ctx_.audio.update_music();
        SDL_SetRenderDrawColor(ctx_.sdl, 0, 0, 0, 255);
        SDL_RenderClear(ctx_.sdl);
        draw_text_entry_dialog(ctx_.sdl, ctx_.front_font, 180.0f, label, entry, "Done", "Cancel");
        SDL_RenderPresent(ctx_.sdl);
        SDL_Delay(2);
    }
    SDL_StopTextInput(ctx_.window);
    return result;
}

}  // namespace bomber::game
