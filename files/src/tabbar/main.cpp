#define WLR_USE_UNSTABLE

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/desktop/view/window/WindowPresentation.hpp>
#include <hyprland/src/desktop/view/window/WindowGroupMembership.hpp>
#include <hyprland/src/desktop/view/Group.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/config/shared/complex/CornersData.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/config/values/types/BoolValue.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/config/values/types/ColorValue.hpp>
#include <hyprland/src/config/values/types/StringValue.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/decorations/IHyprWindowDecoration.hpp>
#include <hyprland/src/render/decorations/DecorationPositioner.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/workspace/HLWorkspace.hpp>
#include <hyprland/src/config/lua/objects/LuaWindow.hpp>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

// The plugin reads the group bar's geometry and members, which upstream keeps private.
#define private public
#include <hyprland/src/render/decorations/CHyprGroupBarDecoration.hpp>
#include <hyprland/src/render/decorations/CHyprBorderDecoration.hpp>
#include <hyprland/src/render/decorations/CHyprDropShadowDecoration.hpp>
#undef private

#include <linux/input-event-codes.h>
#include <cairo/cairo.h>
#include <chrono>
#include <cmath>
#include <format>
#include <unordered_map>

#include "icons.hpp"

using namespace Render;

inline HANDLE PHANDLE = nullptr;

namespace {
    using DrawFn  = void (*)(CHyprGroupBarDecoration*, CRenderContext&, PHLMONITOR, float const&, const SWindowRenderPresentation&);
    using InputFn = bool (*)(CHyprGroupBarDecoration*, const eInputType, const Vector2D&, std::any);
    using PosFn   = SDecorationPositioningInfo (*)(CHyprGroupBarDecoration*);
    using DecoFn  = void (*)(IHyprWindowDecoration*, CRenderContext&, PHLMONITOR, float const&, const SWindowRenderPresentation&);
    using RoundFn = Math::SCornerRadii (*)(Desktop::View::CWindowPresentation*);

    CFunctionHook* g_drawHook   = nullptr;
    CFunctionHook* g_inputHook  = nullptr;
    CFunctionHook* g_posHook    = nullptr;
    CFunctionHook* g_borderHook = nullptr;
    CFunctionHook* g_shadowHook = nullptr;
    CFunctionHook* g_roundHook  = nullptr;

    // The window whose border or shadow is being drawn: its outline also goes around the bar, so it keeps round top corners.
    Desktop::View::CWindow* g_outline = nullptr;

    struct {
        SP<Config::Values::CBoolValue>   enabled;
        SP<Config::Values::CColorValue>  strip, focused, current, inactive, hover, urgent, accent, close, closeHover;
        SP<Config::Values::CIntValue>    accentHeight, iconSize, padding, compactWidth, maxWidth;
        SP<Config::Values::CStringValue> iconTheme;
    } g_config;

    struct STextKey {
        std::string title;
        int         width;
        uint64_t    color;
        int         weight;
        bool        operator==(const STextKey&) const = default;
    };

    struct STextCache {
        STextKey             key;
        SP<Render::ITexture> tex;
    };

    std::unordered_map<std::string, STextCache>   g_text;
    std::unordered_map<int, SP<Render::ITexture>> g_closeTex;
    std::unordered_map<Desktop::View::CWindow*, std::pair<PHLWINDOWREF, std::string>> g_rounded;

    struct {
        PHLWINDOWREF owner;
        int          index   = -1;
        bool         onClose = false;
        std::chrono::steady_clock::time_point since;
    } g_hover;

    SP<CEventLoopTimer> g_marqueeTimer;

    constexpr double MARQUEE_DELAY = 0.6, MARQUEE_HOLD = 1.5, MARQUEE_SPEED = 40;

    PHLWINDOWREF g_pressedClose;

    CHyprColor   color(const SP<Config::Values::CColorValue>& v) {
        return CHyprColor{sc<uint64_t>(v->value())};
    }

    struct SBarGeometry {
        CBox  box;
        float tabWidth = 0;
        float height   = 0;
        float gap      = 0;
        float top      = 0;
    };

    SBarGeometry geometry(CHyprGroupBarDecoration* deco, const CBox& assigned) {
        static auto PHEIGHT          = CConfigValue<Config::INTEGER>("group:groupbar:height");
        static auto PINDICATORGAP    = CConfigValue<Config::INTEGER>("group:groupbar:indicator_gap");
        static auto PINDICATORHEIGHT = CConfigValue<Config::INTEGER>("group:groupbar:indicator_height");
        static auto POUTERGAP        = CConfigValue<Config::INTEGER>("group:groupbar:gaps_out");
        static auto PINNERGAP        = CConfigValue<Config::INTEGER>("group:groupbar:gaps_in");

        SBarGeometry g;
        g.box            = assigned;
        const auto count = std::max<size_t>(1, deco->m_dwGroupMembers.size());
        g.gap            = *PINNERGAP;
        g.tabWidth       = std::min<float>(g_config.maxWidth->value(), (assigned.w - g.gap * (count - 1)) / count);
        g.height         = *PHEIGHT;
        g.top            = assigned.y + assigned.h - (*POUTERGAP + *PINDICATORHEIGHT + *PINDICATORGAP + *PHEIGHT);
        return g;
    }

    double tabsEnd(const SBarGeometry& g, size_t count) {
        return g.box.x + count * (g.tabWidth + g.gap) - g.gap;
    }

    CBox tabBox(const SBarGeometry& g, size_t i) {
        return {g.box.x + i * (g.tabWidth + g.gap), g.top, g.tabWidth, g.height};
    }

    CBox closeBox(const CBox& tab) {
        const double size = std::min(18.0, tab.h - 4);
        return {tab.x + tab.w - size - 5, tab.y + (tab.h - size) / 2.0, size, size};
    }

    bool compact(const CBox& tab) {
        return tab.w < g_config.compactWidth->value();
    }

    Math::SCornerRadii globalRounding() {
        static auto PROUNDING = CConfigValue<Config::IComplexConfigValue>("decoration:rounding");
        return Math::SCornerRadii{*sc<Config::CCornersData*>(PROUNDING.ptr())};
    }

    SP<Render::ITexture> closeTexture(int px, uint64_t rgba) {
        const int key = px;
        if (auto it = g_closeTex.find(key); it != g_closeTex.end())
            return it->second;

        auto*            surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px, px);
        auto*            cr      = cairo_create(surface);
        const CHyprColor c{rgba};
        cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
        cairo_set_line_width(cr, std::max(1.2, px / 9.0));
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        const double m = px * 0.3;
        cairo_move_to(cr, m, m);
        cairo_line_to(cr, px - m, px - m);
        cairo_move_to(cr, px - m, m);
        cairo_line_to(cr, m, px - m);
        cairo_stroke(cr);
        cairo_surface_flush(surface);
        auto tex = g_pHyprRenderer->createTexture(surface);
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
        g_closeTex[key] = tex;
        return tex;
    }

    SP<Render::ITexture> titleTexture(PHLWINDOW w, int maxWidthPx, float scale, bool bright, bool bold, bool full = false) {
        static auto FALLBACKFONT       = CConfigValue<std::string>("misc:font_family");
        static auto PFONTFAMILY        = CConfigValue<std::string>("group:groupbar:font_family");
        static auto PFONTSIZE          = CConfigValue<Config::INTEGER>("group:groupbar:font_size");
        static auto PTEXTCOLOR         = CConfigValue<Config::INTEGER>("group:groupbar:text_color");
        static auto PTEXTCOLORINACTIVE = CConfigValue<Config::INTEGER>("group:groupbar:text_color_inactive");

        const uint64_t rgba   = bright || *PTEXTCOLORINACTIVE == -1 ? *PTEXTCOLOR : *PTEXTCOLORINACTIVE;
        const int      weight = bold ? 600 : 400;
        const STextKey key{w->metadata().title(), maxWidthPx, rgba, weight};
        const auto     id = std::format("{:x}/{}", rc<uintptr_t>(w.get()), (int)full * 4 + (int)bright * 2 + (int)bold);

        if (auto it = g_text.find(id); it != g_text.end() && it->second.key == key)
            return it->second.tex;

        const auto family = *PFONTFAMILY != STRVAL_EMPTY ? *PFONTFAMILY : *FALLBACKFONT;
        auto       tex    = g_pHyprRenderer->renderText(key.title, CHyprColor{rgba}, *PFONTSIZE * scale, false, family, maxWidthPx, weight);
        g_text[id]        = {key, tex};
        return tex;
    }

    void addRect(CRenderContext& ctx, const CBox& box, CHyprColor color, Math::SCornerRadii round, bool blur, const SWindowRenderPresentation& pres,
                 bool xray = false) {
        if (box.empty() || color.a <= 0)
            return;
        CRectPassElement::SRectData data;
        data.box                   = box;
        data.color                 = color;
        data.round                 = round;
        data.blur                  = blur;
        data.xray                  = blur && xray;
        data.workspacePresentation = pres;
        g_pHyprRenderer->addPassElement(ctx, makeUnique<CRectPassElement>(data));
    }

    void addTex(CRenderContext& ctx, SP<Render::ITexture> tex, CBox box, float a, std::optional<CBox> clip = std::nullopt) {
        if (!tex || box.empty())
            return;
        CTexPassElement::SRenderData data;
        data.tex = tex;
        data.box = box.round();
        data.a   = a;
        if (clip)
            data.clipBox = clip->round();
        g_pHyprRenderer->addPassElement(ctx, makeUnique<CTexPassElement>(std::move(data)));
    }

    // Nullopt before the first delay ends (the ellipsized title shows); else the scroll offset in pixels.
    std::optional<double> marqueeOffset(CHyprGroupBarDecoration* self, double overflow, float scale) {
        using namespace std::chrono;
        const double elapsed = duration<double>(steady_clock::now() - g_hover.since).count();
        if (elapsed < MARQUEE_DELAY) {
            g_marqueeTimer->updateTimeout(duration_cast<steady_clock::duration>(duration<double>(MARQUEE_DELAY - elapsed)));
            return std::nullopt;
        }
        const double scroll = overflow / (MARQUEE_SPEED * scale);
        const double cycle  = MARQUEE_DELAY + scroll + MARQUEE_HOLD;
        const double t      = std::fmod(elapsed, cycle);
        if (t < MARQUEE_DELAY) {
            g_marqueeTimer->updateTimeout(duration_cast<steady_clock::duration>(duration<double>(MARQUEE_DELAY - t)));
            return 0.0;
        }
        if (t < MARQUEE_DELAY + scroll) {
            self->damageEntire();
            return std::round((t - MARQUEE_DELAY) * MARQUEE_SPEED * scale);
        }
        g_marqueeTimer->updateTimeout(duration_cast<steady_clock::duration>(duration<double>(cycle - t)));
        return std::round(overflow);
    }

    void hkDraw(CHyprGroupBarDecoration* self, CRenderContext& ctx, PHLMONITOR mon, float const& a, const SWindowRenderPresentation& pres) {
        if (!g_config.enabled->value()) {
            ((DrawFn)g_drawHook->m_original)(self, ctx, mon, a, pres);
            return;
        }

        static auto PBLUR = CConfigValue<Config::BOOL>("group:groupbar:blur");

        const bool  VISIBLE = self->visible();
        if (!self->m_bLastVisibilityStatus.has_value() || VISIBLE != *self->m_bLastVisibilityStatus) {
            g_pDecorationPositioner->repositionDeco(self);
            self->m_bLastVisibilityStatus = VISIBLE;
        }
        if (!VISIBLE || !self->m_window || !self->m_window->grouping().group())
            return;

        const auto ASSIGNED = self->assignedBoxGlobal(pres);
        const auto g        = geometry(self, ASSIGNED);
        self->m_barWidth    = g.tabWidth;
        self->m_barHeight   = ASSIGNED.h;

        const auto  scale    = mon->m_scale;
        const auto  offset   = pres.floatingOffset - mon->m_position;
        const auto  focused  = Desktop::focusState()->window();
        const auto  current  = self->m_window->grouping().group()->current();
        const bool  groupHas = focused && current == focused;
        const auto  R        = globalRounding();
        const auto  count    = self->m_dwGroupMembers.size();
        const bool  blur     = *PBLUR;
        const auto  iconPx   = (int)std::round(g_config.iconSize->value() * scale);
        const auto  padding  = g_config.padding->value();
        const auto& theme    = g_config.iconTheme->value();

        {
            CBox       strip = CBox{g.box.x, g.top, g.box.w, g.height}.translate(offset).scale(scale).round();
            CHyprColor fill  = color(g_config.strip);
            fill.a *= a;
            // A tiled bar has only the wallpaper behind it, so the cached wallpaper blur is enough.
            addRect(ctx, strip, fill, Math::SCornerRadii{R.topLeft * scale, R.topRight * scale, 0.F, 0.F}, blur, pres, !self->m_window->isFloating());
        }

        for (size_t i = 0; i < count; ++i) {
            const auto w = self->m_dwGroupMembers[i].lock();
            if (!w)
                continue;

            const bool isCurrent = w == current;
            const bool isHover   = g_hover.owner == self->m_window && g_hover.index == (int)i;
            const bool isUrgent  = !isCurrent && (w->m_hints & Desktop::View::WINDOW_HINT_URGENT);

            CBox       tab = tabBox(g, i);
            CBox       px  = tab.copy().translate(offset).scale(scale).round();

            Math::SCornerRadii round{0.F};
            if (i == 0)
                round.topLeft = R.topLeft * scale;
            if (i + 1 == count && tab.x + tab.w >= g.box.x + g.box.w - 0.5)
                round.topRight = R.topRight * scale;

            CHyprColor bg = isCurrent ? color(groupHas ? g_config.focused : g_config.current) : color(g_config.inactive);
            if (isHover && !(isCurrent && groupHas))
                bg = color(g_config.hover);
            bg.a *= a;
            addRect(ctx, px, bg, round, false, pres);

            if (isUrgent) {
                CHyprColor tint = color(g_config.urgent);
                tint.a *= a;
                addRect(ctx, px, tint, round, false, pres);
            }

            if (isCurrent && groupHas && g_config.accentHeight->value() > 0) {
                const double h = std::round(g_config.accentHeight->value() * scale);
                CHyprColor   ac = color(g_config.accent);
                ac.a *= a;
                addRect(ctx, CBox{px.x, px.y + px.h - h, px.w, h}, ac, Math::SCornerRadii{0.F}, false, pres);
            }

            const auto icon = appIcon(w->metadata().appID(), iconPx, theme);
            const bool small = compact(tab);
            double     x     = px.x + (small ? (px.w - iconPx) / 2.0 : padding * scale);
            if (icon) {
                const double iconY = px.y + (px.h - iconPx) / 2.0;
                addTex(ctx, icon, CBox{x, iconY, (double)iconPx, (double)iconPx}, a);
                if (isUrgent) {
                    const double     d = std::round(7 * scale);
                    const CHyprColor tint = color(g_config.urgent);
                    addRect(ctx, CBox{x + iconPx - d * 0.6, iconY - d * 0.3, d, d}.round(), CHyprColor{(float)tint.r, (float)tint.g, (float)tint.b, a},
                            Math::SCornerRadii{(float)d / 2}, false, pres);
                }
                x += iconPx + 7 * scale;
            } else if (small)
                x = px.x + 4 * scale;

            if (!small || !icon) {
                const bool   showClose = isHover && !small;
                const double right     = px.x + px.w - (showClose ? 28 : 10) * scale;
                const int    maxW      = std::max(0, (int)(right - x));
                const bool   bright    = isCurrent || isHover || isUrgent;
                const bool   bold      = isCurrent && groupHas;
                auto         text      = maxW > 8 ? titleTexture(w, maxW, scale, bright, bold) : nullptr;
                double       shift     = 0;
                std::optional<CBox> clip;
                if (text && isHover) {
                    const auto full = titleTexture(w, 0, scale, bright, bold, true);
                    if (full && full->m_size.x > maxW)
                        if (const auto offset = marqueeOffset(self, full->m_size.x - maxW, scale)) {
                            text  = full;
                            shift = *offset;
                            clip  = CBox{x, px.y, (double)maxW, px.h};
                        }
                }
                if (text)
                    addTex(ctx, text, CBox{x - shift, px.y + std::round((px.h - text->m_size.y) / 2.0), text->m_size.x, text->m_size.y}, a, clip);
            }

            if (isHover && !small) {
                const auto cb  = closeBox(tab).translate(offset).scale(scale).round();
                if (g_hover.onClose)
                    addRect(ctx, cb, color(g_config.closeHover), Math::SCornerRadii{(float)(5 * scale)}, false, pres);
                addTex(ctx, closeTexture((int)cb.w, g_config.close->value()), cb, a);
            }
        }
    }

    struct SHit {
        CHyprGroupBarDecoration* deco = nullptr;
        int                      index = -1;
        bool                     onClose = false;
    };

    SHit hitTest(CHyprGroupBarDecoration* deco, const Vector2D& pos) {
        if (!deco || !deco->visible() || !deco->m_window)
            return {};
        const auto g = geometry(deco, deco->assignedBoxGlobal());
        for (size_t i = 0; i < deco->m_dwGroupMembers.size(); ++i) {
            const auto tab = tabBox(g, i);
            if (!tab.containsPoint(pos))
                continue;
            return {deco, (int)i, !compact(tab) && closeBox(tab).containsPoint(pos)};
        }
        return {};
    }

    void damageOwner(const PHLWINDOWREF& owner) {
        if (!owner)
            return;
        if (const auto deco = owner->presentation().decoration(DECORATION_GROUPBAR))
            deco->damageEntire();
    }

    void setHover(PHLWINDOWREF owner, int index, bool onClose) {
        if (g_hover.owner == owner && g_hover.index == index && g_hover.onClose == onClose)
            return;
        const auto old   = g_hover.owner;
        const bool moved = g_hover.owner != owner || g_hover.index != index;
        g_hover          = {owner, index, onClose, moved ? std::chrono::steady_clock::now() : g_hover.since};
        damageOwner(old);
        if (owner != old)
            damageOwner(owner);
    }

    std::pair<PHLWINDOW, SHit> barAt(const Vector2D& pos) {
        for (const auto& w : Desktop::windowState()->windows()) {
            if (!w->mapped() || w->isHidden() || !w->m_workspace || !w->m_workspace->visible() || !w->grouping().group() || w->grouping().group()->current() != w)
                continue;
            const auto deco = w->presentation().decoration(DECORATION_GROUPBAR);
            if (!deco)
                continue;
            const auto hit = hitTest(sc<CHyprGroupBarDecoration*>(deco.get()), pos);
            if (hit.deco)
                return {w, hit};
        }
        return {nullptr, {}};
    }

    void onMouseMove(const Vector2D& pos) {
        if (!g_config.enabled->value())
            return;
        const auto [owner, hit] = barAt(pos);
        if (owner)
            setHover(owner, hit.index, hit.onClose);
        else
            setHover({}, -1, false);
    }

    int luaTabAt(lua_State* L) {
        const Vector2D pos{luaL_checknumber(L, 1), luaL_checknumber(L, 2)};
        const auto [owner, hit] = barAt(pos);
        const auto tab          = owner ? owner->grouping().group()->fromIndex(hit.index) : nullptr;
        if (!tab) {
            lua_pushnil(L);
            return 1;
        }
        Config::Lua::Objects::CLuaWindow::push(L, tab);
        return 1;
    }

    bool hkInput(CHyprGroupBarDecoration* self, const eInputType type, const Vector2D& pos, std::any data) {
        const auto original = (InputFn)g_inputHook->m_original;
        if (!g_config.enabled->value() || (type != INPUT_TYPE_BUTTON && type != INPUT_TYPE_DRAG_START && type != INPUT_TYPE_DRAG_END))
            return original(self, type, pos, data);

        if (type == INPUT_TYPE_DRAG_END || type == INPUT_TYPE_BUTTON || type == INPUT_TYPE_DRAG_START) {
            const auto g   = geometry(self, self->assignedBoxGlobal());
            const auto end = tabsEnd(g, self->m_dwGroupMembers.size());
            if (pos.x > end) {
                if (type == INPUT_TYPE_DRAG_END)
                    return original(self, type, Vector2D{end - 1, pos.y}, data);
                if (type == INPUT_TYPE_DRAG_START)
                    return false;
                const auto owner = self->m_window.lock();
                if (owner && !Desktop::focusState()->isWindowActive(owner))
                    Desktop::focusState()->rawWindowFocus(owner, Desktop::FOCUS_REASON_CLICK);
                return true;
            }
        }

        const auto hit = hitTest(self, pos);
        if (type == INPUT_TYPE_DRAG_START)
            return hit.onClose ? true : original(self, type, pos, data);
        if (type == INPUT_TYPE_DRAG_END)
            return original(self, type, pos, data);

        const auto e = std::any_cast<const IPointer::SButtonEvent&>(data);
        if (e.button != BTN_LEFT)
            return original(self, type, pos, data);

        if (e.state == WL_POINTER_BUTTON_STATE_PRESSED) {
            if (!hit.onClose) {
                g_pressedClose.reset();
                return original(self, type, pos, data);
            }
            g_pressedClose = self->m_window->grouping().group()->fromIndex(hit.index);
            return true;
        }

        if (g_pressedClose) {
            const auto target = g_pressedClose.lock();
            g_pressedClose.reset();
            if (hit.onClose && target && target == self->m_window->grouping().group()->fromIndex(hit.index))
                target->sendClose();
            return true;
        }
        return original(self, type, pos, data);
    }

    std::string squareTop() {
        const auto r = globalRounding();
        return std::format("0 0 {} {}", (int)r.bottomRight, (int)r.bottomLeft);
    }

    void syncCorners() {
        const bool enabled = g_config.enabled->value();
        const auto value   = squareTop();

        for (auto it = g_rounded.begin(); it != g_rounded.end();) {
            if (!it->second.first)
                it = g_rounded.erase(it);
            else
                ++it;
        }

        for (const auto& w : Desktop::windowState()->windows()) {
            const auto deco = w->presentation().decoration(DECORATION_GROUPBAR);
            const bool want = enabled && w->mapped() && deco && sc<CHyprGroupBarDecoration*>(deco.get())->visible();
            const auto it   = g_rounded.find(w.get());

            if (want && (it == g_rounded.end() || it->second.second != value)) {
                (void)Config::Actions::setProp("rounding", value, w);
                g_rounded[w.get()] = {w, value};
            } else if (!want && it != g_rounded.end()) {
                (void)Config::Actions::setProp("rounding", "unset", w);
                g_rounded.erase(it);
            }
        }
    }

    SDecorationPositioningInfo hkPosInfo(CHyprGroupBarDecoration* self) {
        auto info = ((PosFn)g_posHook->m_original)(self);
        if (g_config.enabled->value())
            info.priority = 10005;
        return info;
    }

    template <typename T>
    void outlineDraw(CFunctionHook* hook, T* self, CRenderContext& ctx, PHLMONITOR mon, float const& a, const SWindowRenderPresentation& pres) {
        const auto w = self->m_window.lock();
        g_outline    = w && g_rounded.contains(w.get()) ? w.get() : nullptr;
        ((DecoFn)hook->m_original)(self, ctx, mon, a, pres);
        g_outline = nullptr;
    }

    void hkBorderDraw(CHyprBorderDecoration* self, CRenderContext& ctx, PHLMONITOR mon, float const& a, const SWindowRenderPresentation& pres) {
        outlineDraw(g_borderHook, self, ctx, mon, a, pres);
    }

    void hkShadowDraw(CHyprDropShadowDecoration* self, CRenderContext& ctx, PHLMONITOR mon, float const& a, const SWindowRenderPresentation& pres) {
        outlineDraw(g_shadowHook, self, ctx, mon, a, pres);
    }

    Math::SCornerRadii hkRounding(Desktop::View::CWindowPresentation* self) {
        auto r = ((RoundFn)g_roundHook->m_original)(self);
        if (g_outline && &g_outline->presentation() == self) {
            const auto global = globalRounding();
            r.topLeft         = global.topLeft;
            r.topRight        = global.topRight;
        }
        return r;
    }

    void forgetWindow(Desktop::View::CWindow* w) {
        g_rounded.erase(w);
        for (auto it = g_text.begin(); it != g_text.end();) {
            if (it->first.starts_with(std::format("{:x}/", rc<uintptr_t>(w))))
                it = g_text.erase(it);
            else
                ++it;
        }
    }

    void damageGroupOf(PHLWINDOW w) {
        if (!w || !w->grouping().group())
            return;
        if (const auto cur = w->grouping().group()->current())
            damageOwner(cur);
    }

    void* findMember(const std::string& name, const std::string& demangledPrefix) {
        for (const auto& m : HyprlandAPI::findFunctionsByName(PHANDLE, name)) {
            if (m.demangled.starts_with(demangledPrefix))
                return m.address;
        }
        return nullptr;
    }
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    if (std::string{__hyprland_api_get_hash()} != __hyprland_api_get_client_hash()) {
        HyprlandAPI::addNotification(PHANDLE, "[tabbar] Not loaded: built for a different Hyprland", CHyprColor{1.0, 0.2, 0.2, 1.0}, 8000);
        throw std::runtime_error("[tabbar] version mismatch");
    }

    using namespace Config::Values;
    g_config.enabled      = makeShared<CBoolValue>("plugin:tabbar:enabled", "Draw the group tab bars", true);
    g_config.strip        = makeShared<CColorValue>("plugin:tabbar:col.strip", "Bar behind the tabs", 0xa6292522);
    g_config.focused      = makeShared<CColorValue>("plugin:tabbar:col.focused", "Current tab of the focused group", 0x29ece1d7);
    g_config.current      = makeShared<CColorValue>("plugin:tabbar:col.current", "Current tab of another group", 0x12ece1d7);
    g_config.inactive     = makeShared<CColorValue>("plugin:tabbar:col.inactive", "Other tabs", 0x00000000);
    g_config.hover        = makeShared<CColorValue>("plugin:tabbar:col.hover", "Tab under the pointer", 0x1fffffff);
    g_config.urgent       = makeShared<CColorValue>("plugin:tabbar:col.urgent", "Tint of a tab that asks for attention", 0x40d47766);
    g_config.accent       = makeShared<CColorValue>("plugin:tabbar:col.accent", "Line under the current tab of the focused group", 0xffe49b5d);
    g_config.close        = makeShared<CColorValue>("plugin:tabbar:col.close", "Close button", 0xffece1d7);
    g_config.closeHover   = makeShared<CColorValue>("plugin:tabbar:col.close_hover", "Behind the close button under the pointer", 0x29ece1d7);
    g_config.accentHeight = makeShared<CIntValue>("plugin:tabbar:accent_height", "Height of the accent line, 0 = none", 2);
    g_config.iconSize     = makeShared<CIntValue>("plugin:tabbar:icon_size", "App icon size", 16);
    g_config.padding      = makeShared<CIntValue>("plugin:tabbar:padding", "Space left of the icon", 10);
    g_config.compactWidth = makeShared<CIntValue>("plugin:tabbar:compact_width", "Below this tab width only the icon shows", 90);
    g_config.maxWidth     = makeShared<CIntValue>("plugin:tabbar:max_width", "Largest tab width", 240);
    g_config.iconTheme    = makeShared<CStringValue>("plugin:tabbar:icon_theme", "Icon theme, hicolor is the fallback", "Adwaita");

    for (const auto& v : std::initializer_list<SP<IValue>>{g_config.enabled, g_config.strip, g_config.focused, g_config.current, g_config.inactive, g_config.hover, g_config.urgent,
                                                           g_config.accent, g_config.close, g_config.closeHover, g_config.accentHeight, g_config.iconSize, g_config.padding,
                                                           g_config.compactWidth, g_config.maxWidth, g_config.iconTheme})
        HyprlandAPI::addConfigValueV2(PHANDLE, v);

    const std::vector<std::tuple<CFunctionHook**, const char*, const char*, void*>> hooks = {
        {&g_drawHook, "draw", "CHyprGroupBarDecoration::draw(", (void*)&hkDraw},
        {&g_inputHook, "onInputOnDeco", "CHyprGroupBarDecoration::onInputOnDeco(", (void*)&hkInput},
        {&g_posHook, "getPositioningInfo", "CHyprGroupBarDecoration::getPositioningInfo(", (void*)&hkPosInfo},
        {&g_borderHook, "draw", "CHyprBorderDecoration::draw(", (void*)&hkBorderDraw},
        {&g_shadowHook, "draw", "CHyprDropShadowDecoration::draw(", (void*)&hkShadowDraw},
        {&g_roundHook, "rounding", "Desktop::View::CWindowPresentation::rounding(", (void*)&hkRounding},
    };
    for (const auto& [slot, name, prefix, target] : hooks) {
        const auto addr = findMember(name, prefix);
        if (!addr)
            throw std::runtime_error(std::format("[tabbar] {} not found", prefix));
        *slot = HyprlandAPI::createFunctionHook(PHANDLE, addr, target);
        if (!*slot || !(*slot)->hook())
            throw std::runtime_error(std::format("[tabbar] cannot hook {}", prefix));
    }

    static auto P1 = Event::bus()->m_events.input.mouse.move.listen([](Vector2D pos, Event::SCallbackInfo&) { onMouseMove(pos); });
    static auto P2 = Event::bus()->m_events.render.pre.listen([](PHLMONITOR) { syncCorners(); });
    static auto P3 = Event::bus()->m_events.window.destroy.listen([](PHLWINDOWREF w) { forgetWindow(w.get()); });
    static auto P4 = Event::bus()->m_events.window.urgent.listen([](PHLWINDOW w) { damageGroupOf(w); });
    static auto P5 = Event::bus()->m_events.window.title.listen([](PHLWINDOW w) { damageGroupOf(w); });
    static auto P6 = Event::bus()->m_events.config.reloaded.listen([] {
        clearIconCache();
        g_closeTex.clear();
        g_text.clear();
    });

    g_marqueeTimer = makeShared<CEventLoopTimer>(std::nullopt, [](SP<CEventLoopTimer>, void*) { damageOwner(g_hover.owner); }, nullptr);
    g_pEventLoopManager->addTimer(g_marqueeTimer);

    HyprlandAPI::addLuaFunction(PHANDLE, "tabbar", "tab_at", luaTabAt);

    HyprlandAPI::reloadConfig();

    return {"tabbar", "Group tab bars with app icons, hover, close button and per-corner rounding", "hadi", "0.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    if (g_marqueeTimer) {
        g_pEventLoopManager->removeTimer(g_marqueeTimer);
        g_marqueeTimer.reset();
    }
    for (auto& [ptr, entry] : g_rounded) {
        if (const auto w = entry.first.lock())
            (void)Config::Actions::setProp("rounding", "unset", w);
    }
    g_rounded.clear();
    g_text.clear();
    g_closeTex.clear();
    clearIconCache();
}
