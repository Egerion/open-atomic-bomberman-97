#include "bomber/game/sprites.hpp"

#include <algorithm>
#include <utility>

namespace bomber::game {

SDL_Texture* make_texture(SDL_Renderer* ren, const assets::Image& img) {
    SDL_Surface* surf = SDL_CreateSurfaceFrom(img.width, img.height, SDL_PIXELFORMAT_RGBA32,
                                              const_cast<std::uint8_t*>(img.rgba.data()),
                                              img.width * 4);
    if (!surf) return nullptr;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(ren, surf);
    SDL_DestroySurface(surf);
    if (tex) SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
    return tex;
}

assets::Image recolor_image(assets::Image img, const std::int32_t rgb[3]) {
    for (std::size_t i = 0; i + 3 < img.rgba.size(); i += 4) {
        std::uint8_t r = img.rgba[i], g = img.rgba[i + 1], b = img.rgba[i + 2];
        if (img.rgba[i + 3] == 0) continue;
        if (g > r + 24 && g > b + 24) {
            int lum = g;
            // Keep specular glints alive on dark target colors (black player
            // must stay readable on dark stages).
            int glint = lum > 176 ? (lum - 176) : 0;
            img.rgba[i + 0] = static_cast<std::uint8_t>(std::min(255, lum * rgb[0] / 100 + glint));
            img.rgba[i + 1] = static_cast<std::uint8_t>(std::min(255, lum * rgb[1] / 100 + glint));
            img.rgba[i + 2] = static_cast<std::uint8_t>(std::min(255, lum * rgb[2] / 100 + glint));
        }
    }
    return img;
}

void AniTextures::load(SDL_Renderer* ren, const std::filesystem::path& path) {
    reset();
    data_ = assets::ani::load(path);
    textures_.assign(data_.frames.size(), nullptr);
    for (std::size_t i = 0; i < data_.frames.size(); ++i)
        if (!data_.frames[i].image.empty())
            textures_[i] = make_texture(ren, data_.frames[i].image);
}

AniTextures AniTextures::recolored(SDL_Renderer* ren, const std::int32_t rgb[3]) const {
    AniTextures out;
    out.data_ = data_;
    out.textures_.assign(out.data_.frames.size(), nullptr);
    for (std::size_t i = 0; i < out.data_.frames.size(); ++i) {
        auto& f = out.data_.frames[i];
        if (f.image.empty()) continue;
        f.image = recolor_image(std::move(f.image), rgb);
        out.textures_[i] = make_texture(ren, f.image);
    }
    return out;
}

void AniTextures::reset() {
    for (auto* t : textures_)
        if (t) SDL_DestroyTexture(t);
    textures_.clear();
    data_ = {};
}

Anim resolve_sequence(const AniTextures& ani, const std::string& name) {
    Anim out;
    for (const auto& s : ani.data().sequences) {
        if (s.name != name) continue;
        for (const auto& st : s.steps) {
            if (st.frame < 0) continue;
            const auto& f = ani.data().frames[static_cast<std::size_t>(st.frame)];
            out.steps.push_back({ani.texture(static_cast<std::size_t>(st.frame)), f.image.width,
                                 f.image.height, f.hotspot_x - st.dx, f.hotspot_y - st.dy});
        }
        break;
    }
    return out;
}

void collect_death_anims(const AniTextures& ani, std::vector<Anim>& out) {
    for (const auto& sq : ani.data().sequences) {
        if (sq.name.rfind("die", 0) != 0) continue;
        Anim a;
        for (const auto& st : sq.steps) {
            if (st.frame < 0) continue;
            const auto& f = ani.data().frames[static_cast<std::size_t>(st.frame)];
            a.steps.push_back({ani.texture(static_cast<std::size_t>(st.frame)), f.image.width,
                               f.image.height, f.hotspot_x - st.dx, f.hotspot_y - st.dy});
        }
        if (!a.steps.empty()) out.push_back(std::move(a));
    }
}

}  // namespace bomber::game
