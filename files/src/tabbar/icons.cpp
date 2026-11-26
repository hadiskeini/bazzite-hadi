#include "icons.hpp"

#include <hyprland/src/render/Renderer.hpp>
#include <hyprgraphics/image/Image.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
using Clock  = std::chrono::steady_clock;

namespace {
    struct SDesktopIndex {
        std::unordered_map<std::string, std::string> byID, byWMClass;
        Clock::time_point                            built;
        bool                                         valid = false;
    };

    struct SCached {
        SP<Render::ITexture> tex;
        Clock::time_point    at;
    };

    SDesktopIndex                            g_index;
    std::unordered_map<std::string, SCached> g_icons;

    constexpr auto                           RETRY = std::chrono::seconds(30);

    std::string                              lower(std::string s) {
        std::ranges::transform(s, s.begin(), [](unsigned char c) { return std::tolower(c); });
        return s;
    }

    std::vector<fs::path> dataDirs() {
        std::vector<fs::path> dirs;
        const char*           home    = std::getenv("HOME");
        const char*           dataHome = std::getenv("XDG_DATA_HOME");
        if (dataHome && *dataHome)
            dirs.emplace_back(dataHome);
        else if (home)
            dirs.emplace_back(fs::path(home) / ".local/share");

        const char*       env = std::getenv("XDG_DATA_DIRS");
        const std::string all = env && *env ? env : "/usr/local/share:/usr/share";
        size_t            start = 0;
        while (start <= all.size()) {
            const size_t end = all.find(':', start);
            const auto   part = all.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (!part.empty())
                dirs.emplace_back(part);
            if (end == std::string::npos)
                break;
            start = end + 1;
        }

        for (const fs::path& extra : {fs::path("/var/lib/flatpak/exports/share"), home ? fs::path(home) / ".local/share/flatpak/exports/share" : fs::path()}) {
            if (!extra.empty() && std::ranges::find(dirs, extra) == dirs.end())
                dirs.push_back(extra);
        }
        return dirs;
    }

    void buildIndex() {
        g_index       = {};
        g_index.built = Clock::now();
        g_index.valid = true;

        std::error_code ec;
        for (const auto& base : dataDirs()) {
            const auto apps = base / "applications";
            for (const auto& entry : fs::directory_iterator(apps, ec)) {
                if (entry.path().extension() != ".desktop")
                    continue;

                std::ifstream file(entry.path());
                std::string   line, icon, wmClass;
                bool          inMain = false;
                while (std::getline(file, line)) {
                    if (line.starts_with('[')) {
                        inMain = line == "[Desktop Entry]";
                        continue;
                    }
                    if (!inMain)
                        continue;
                    if (line.starts_with("Icon="))
                        icon = line.substr(5);
                    else if (line.starts_with("StartupWMClass="))
                        wmClass = line.substr(15);
                }
                if (icon.empty())
                    continue;

                const auto id = lower(entry.path().stem().string());
                g_index.byID.try_emplace(id, icon);
                if (!wmClass.empty())
                    g_index.byWMClass.try_emplace(lower(wmClass), icon);
            }
        }
    }

    std::optional<std::string> iconName(const std::string& appID) {
        const auto key = lower(appID);
        for (int pass = 0; pass < 2; ++pass) {
            if (!g_index.valid || (pass == 1 && Clock::now() - g_index.built > RETRY))
                buildIndex();
            else if (pass == 1)
                break;

            if (auto it = g_index.byWMClass.find(key); it != g_index.byWMClass.end())
                return it->second;
            if (auto it = g_index.byID.find(key); it != g_index.byID.end())
                return it->second;
            const auto dot = key.rfind('.');
            if (dot != std::string::npos) {
                if (auto it = g_index.byID.find(key.substr(dot + 1)); it != g_index.byID.end())
                    return it->second;
            }
            for (const auto& [id, icon] : g_index.byID) {
                if (id.ends_with("." + key))
                    return icon;
            }
        }
        return std::nullopt;
    }

    int sizeOf(const fs::path& path) {
        for (const auto& part : path) {
            const auto s = part.string();
            if (s == "scalable")
                return 100000;
            const auto x = s.find('x');
            const auto n = std::atoi(s.substr(0, x).c_str());
            if (n > 0)
                return n;
        }
        return 0;
    }

    std::vector<std::string> themeChain(const std::string& theme) {
        std::vector<std::string> chain{theme};
        for (const auto& base : dataDirs()) {
            std::ifstream file(base / "icons" / theme / "index.theme");
            std::string   line;
            while (std::getline(file, line)) {
                if (!line.starts_with("Inherits="))
                    continue;
                std::string rest = line.substr(9);
                size_t      start = 0;
                while (start < rest.size()) {
                    const auto end  = rest.find(',', start);
                    const auto name = rest.substr(start, end == std::string::npos ? std::string::npos : end - start);
                    if (!name.empty() && std::ranges::find(chain, name) == chain.end())
                        chain.push_back(name);
                    if (end == std::string::npos)
                        break;
                    start = end + 1;
                }
            }
            if (chain.size() > 1)
                break;
        }
        if (std::ranges::find(chain, "hicolor") == chain.end())
            chain.emplace_back("hicolor");
        return chain;
    }

    std::optional<fs::path> findIcon(const std::string& name, int px, const std::string& theme) {
        if (name.starts_with('/'))
            return fs::exists(name) ? std::optional<fs::path>(name) : std::nullopt;

        std::error_code ec;
        for (const auto& themeName : themeChain(theme)) {
            std::optional<fs::path> best;
            int                     bestScore = -1;
            for (const auto& base : dataDirs()) {
                const auto root = base / "icons" / themeName;
                if (!fs::is_directory(root, ec))
                    continue;
                for (auto it = fs::recursive_directory_iterator(root, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
                    if (ec)
                        break;
                    if (it.depth() > 3) {
                        it.disable_recursion_pending();
                        continue;
                    }
                    const auto& p = it->path();
                    if (p.stem() != name || (p.extension() != ".svg" && p.extension() != ".png"))
                        continue;
                    const int size  = sizeOf(fs::relative(p, root, ec));
                    const int score = size >= px ? 200000 - size : size;
                    if (score > bestScore) {
                        bestScore = score;
                        best      = p;
                    }
                }
            }
            if (best)
                return best;
        }

        for (const auto& base : dataDirs()) {
            for (const char* ext : {".svg", ".png"}) {
                const auto p = base / "pixmaps" / (name + ext);
                if (fs::exists(p, ec))
                    return p;
            }
        }
        return std::nullopt;
    }

    SP<Render::ITexture> load(const fs::path& path, int px) {
        Hyprgraphics::CImage image(path.string(), Hyprutils::Math::Vector2D{(double)px, (double)px});
        if (!image.success())
            return nullptr;

        const auto src  = image.cairoSurface();
        const auto size = src->size();
        if (size.x <= 0 || size.y <= 0)
            return nullptr;

        auto*      surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px, px);
        auto*      cr      = cairo_create(surface);
        const auto scale   = std::min(px / size.x, px / size.y);
        cairo_translate(cr, (px - size.x * scale) / 2.0, (px - size.y * scale) / 2.0);
        cairo_scale(cr, scale, scale);
        cairo_set_source_surface(cr, src->cairo(), 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
        cairo_paint(cr);
        cairo_surface_flush(surface);

        auto tex = g_pHyprRenderer->createTexture(surface);
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
        return tex;
    }
}

SP<Render::ITexture> appIcon(const std::string& appID, int px, const std::string& theme) {
    if (appID.empty() || px <= 0)
        return nullptr;

    const auto key = appID + "@" + std::to_string(px);
    if (auto it = g_icons.find(key); it != g_icons.end() && (it->second.tex || Clock::now() - it->second.at < RETRY))
        return it->second.tex;

    SP<Render::ITexture> tex;
    for (const auto& name : {iconName(appID), std::optional<std::string>(lower(appID))}) {
        if (!name)
            continue;
        if (const auto path = findIcon(*name, px, theme)) {
            tex = load(*path, px);
            if (tex)
                break;
        }
    }

    g_icons[key] = {tex, Clock::now()};
    return tex;
}

void clearIconCache() {
    g_icons.clear();
    g_index = {};
}
