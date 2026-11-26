#pragma once

#include <hyprland/src/render/Texture.hpp>
#include <string>

SP<Render::ITexture> appIcon(const std::string& appID, int px, const std::string& theme);
void                 clearIconCache();
