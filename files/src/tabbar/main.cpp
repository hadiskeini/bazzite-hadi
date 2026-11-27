#define WLR_USE_UNSTABLE

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/desktop/view/window/WindowPresentation.hpp>
#include <hyprland/src/desktop/view/window/WindowGroupMembership.hpp>
#include <hyprland/src/desktop/view/Group.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/rule/windowRule/WindowRuleApplicator.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/config/shared/complex/CornersData.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/config/values/types/BoolValue.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/config/values/types/ColorValue.hpp>
#include <hyprland/src/config/values/types/StringValue.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/pass/BorderPassElement.hpp>
#include <hyprland/src/render/decorations/IHyprWindowDecoration.hpp>
#include <hyprland/src/render/decorations/DecorationPositioner.hpp>
#include <hyprland/src/render/decorations/AnimatedDecorationGradient.hpp>
#include <hyprland/src/render/decorations/CHyprBorderDecoration.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/ipc/s2/S2.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/supplementary/DragController.hpp>
#include <hyprland/src/layout/target/WindowTarget.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/workspace/HLWorkspace.hpp>
#include <hyprland/src/config/lua/objects/LuaWindow.hpp>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

#include <linux/input-event-codes.h>
#include <cairo/cairo.h>
#include <chrono>
#include <cmath>
#include <format>
#include <unordered_map>

#include "icons.hpp"

using namespace Render;
using Desktop::Types::PRIORITY_SET_PROP;

inline HANDLE PHANDLE = nullptr;

namespace {
    struct {
        SP<Config::Values::CBoolValue>   enabled;
        SP<Config::Values::CColorValue>  strip, focused, current, inactive, hover, urgent, accent, close, closeHover;
        SP<Config::Values::CIntValue>    accentHeight, iconSize, padding, compactWidth, maxWidth;
        SP<Config::Values::CStringValue> iconTheme;
        SP<Config::Values::CFloatValue>  tiledShadow, tiledShadowAlpha;
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

    struct {
        PHLWINDOWREF owner;
        int          index   = -1;
        bool         onClose = false;
        std::chrono::steady_clock::time_point since;
    } g_hover;

    SP<CEventLoopTimer> g_marqueeTimer;

    // A plain drag of a tab; tabs.lua acts on the release, the plugin only draws it.
    struct {
        PHLWINDOWREF tab;
        Vector2D     press, grab, size;
        bool         active = false;
    } g_drag;
    constexpr double DRAG_MIN = 10;

    constexpr double MARQUEE_DELAY = 0.6, MARQUEE_HOLD = 1.5, MARQUEE_SPEED = 40;

    PHLWINDOWREF g_pressedClose;

    CHyprColor   color(const SP<Config::Values::CColorValue>& v) {
        return CHyprColor{sc<uint64_t>(v->value())};
    }

    Math::SCornerRadii globalRounding() {
        static auto PROUNDING = CConfigValue<Config::IComplexConfigValue>("decoration:rounding");
        return Math::SCornerRadii{*sc<Config::CCornersData*>(PROUNDING.ptr())};
    }

    // The plugin sets these props itself; its own border and shadow follow the user's rules only.
    template <typename T>
    Desktop::Types::COverridableVar<T> withoutOwnProp(const Desktop::Types::COverridableVar<T>& var) {
        auto copy = var;
        copy.unset(PRIORITY_SET_PROP);
        return copy;
    }

    class CTabBar : public IHyprWindowDecoration {
      public:
        explicit CTabBar(PHLWINDOW window) : IHyprWindowDecoration(window), m_owner(window) {}

        SDecorationPositioningInfo getPositioningInfo() override;
        void                       onPositioningReply(const SDecorationPositioningReply& reply) override {
            m_assigned = reply.assignedGeometry;
        }
        void            draw(CRenderContext& ctx, PHLMONITOR mon, float const& a, const SWindowRenderPresentation& pres) override;
        eDecorationType getDecorationType() override {
            return DECORATION_CUSTOM;
        }
        void             updateWindow(PHLWINDOW) override;
        void             damageEntire() override;
        bool             onInputOnDeco(const eInputType type, const Vector2D& pos, std::any data) override;
        eDecorationLayer getDecorationLayer() override {
            return DECORATION_LAYER_OVER;
        }
        // Part of the window: the shadow and the border go around the bar too.
        uint64_t getDecorationFlags() override {
            return DECORATION_ALLOWS_MOUSE_INPUT | DECORATION_PART_OF_MAIN_WINDOW;
        }
        std::string getDisplayName() override {
            return "tabbar";
        }

        bool visible() const;
        CBox assignedBoxGlobal() const;
        CBox assignedBoxGlobal(const SWindowRenderPresentation& pres) const;

        PHLWINDOWREF              m_owner;
        std::vector<PHLWINDOWREF> m_members;

      private:
        void reposition();

        CBox                m_assigned;
        std::optional<bool> m_lastVisible;
    };

    class CTabBorder : public IHyprWindowDecoration {
      public:
        explicit CTabBorder(PHLWINDOW window) : IHyprWindowDecoration(window), m_owner(window), m_gradient(Config::CGradientValueData{CHyprColor(sc<uint64_t>(0))}) {}

        SDecorationPositioningInfo getPositioningInfo() override {
            return {};
        }
        void            onPositioningReply(const SDecorationPositioningReply&) override {}
        void            draw(CRenderContext& ctx, PHLMONITOR mon, float const& a, const SWindowRenderPresentation& pres) override;
        eDecorationType getDecorationType() override {
            return DECORATION_CUSTOM;
        }
        void             updateWindow(PHLWINDOW) override {}
        void             damageEntire() override;
        eDecorationLayer getDecorationLayer() override {
            return DECORATION_LAYER_OVER;
        }
        uint64_t getDecorationFlags() override {
            return 0;
        }
        std::string getDisplayName() override {
            return "tabbar border";
        }
        void initializeAnimations() override {
            m_gradient.initializeAnimations(m_owner.lock(), self(), "border", "borderangle");
        }
        void updateState() override;
        void onWindowMap() override {
            m_gradient.onWindowMap();
        }
        void onWindowFocus() override {
            m_gradient.onWindowFocus();
        }

      private:
        CHyprBorderDecoration* hyprlandBorder() const;

        PHLWINDOWREF                m_owner;
        CAnimatedDecorationGradient m_gradient;
    };

    class CTabShadow : public IHyprWindowDecoration {
      public:
        explicit CTabShadow(PHLWINDOW window) : IHyprWindowDecoration(window), m_owner(window) {}

        SDecorationPositioningInfo getPositioningInfo() override;
        void                       onPositioningReply(const SDecorationPositioningReply&) override {
            updateWindow(m_owner.lock());
        }
        void            draw(CRenderContext& ctx, PHLMONITOR mon, float const& a, const SWindowRenderPresentation& pres) override;
        eDecorationType getDecorationType() override {
            return DECORATION_CUSTOM;
        }
        void             updateWindow(PHLWINDOW) override;
        void             damageEntire() override;
        eDecorationLayer getDecorationLayer() override {
            return DECORATION_LAYER_BOTTOM;
        }
        uint64_t getDecorationFlags() override {
            return DECORATION_NON_SOLID;
        }
        std::string getDisplayName() override {
            return "tabbar shadow";
        }
        void initializeAnimations() override {
            m_gradient.initializeAnimations(m_owner.lock(), self(), "fadeShadow", "shadowangle");
        }
        void updateState() override;
        void onWindowMap() override {
            m_gradient.onWindowMap();
        }
        void onWindowFocus() override {
            m_gradient.onWindowFocus();
        }

        void render(CRenderContext& ctx, PHLMONITOR mon, float a, const SWindowRenderPresentation& pres);

      private:
        bool                        canRender() const;

        PHLWINDOWREF                m_owner;
        CAnimatedDecorationGradient m_gradient;
        SBoxExtents                 m_extents, m_reportedExtents;
        Vector2D                    m_lastPos, m_lastSize;
        CBox                        m_lastBoxWithDecos;
    };

    // Hyprland's shadow element calls back only into its own decoration, so the shadow has its own element.
    class CTabShadowPass : public IPassElement {
      public:
        CTabShadowPass(WP<CTabShadow> deco, float a, const SWindowRenderPresentation& pres) : m_deco(deco), m_a(a), m_pres(pres) {}

        std::vector<UP<IPassElement>> draw(CRenderContext& ctx) override {
            if (const auto deco = m_deco.lock())
                deco->render(ctx, ctx.m_data.pMonitor.lock(), m_a, m_pres);
            return {};
        }
        bool needsLiveBlur(CRenderContext&) override {
            return false;
        }
        bool needsPrecomputeBlur(CRenderContext&) override {
            return false;
        }
        const char* passName() override {
            return "CTabShadowPass";
        }
        ePassElementType type() override {
            return EK_CUSTOM;
        }

      private:
        WP<CTabShadow>            m_deco;
        float                     m_a;
        SWindowRenderPresentation m_pres;
    };

    struct SDecorations {
        WP<CTabBar>    bar;
        WP<CTabBorder> border;
        WP<CTabShadow> shadow;
    };
    std::unordered_map<Desktop::View::CWindow*, SDecorations> g_decorations;

    // What the plugin changed on a window: its shadow replaces Hyprland's, and a window with a bar gets the plugin's border and square top corners.
    struct SApplied {
        PHLWINDOWREF window;
        bool         shadow = false;
        std::string  rounding;
    };
    std::unordered_map<Desktop::View::CWindow*, SApplied> g_applied;

    CTabBar* barOf(const PHLWINDOW& w) {
        if (!w)
            return nullptr;
        const auto it = g_decorations.find(w.get());
        return it == g_decorations.end() ? nullptr : it->second.bar.get();
    }

    bool hasBar(const PHLWINDOW& w) {
        const auto bar = barOf(w);
        return bar && bar->visible();
    }

    void attach(const PHLWINDOW& w) {
        if (!validMapped(w) || barOf(w))
            return;
        auto shadow = makeShared<CTabShadow>(w);
        auto bar    = makeShared<CTabBar>(w);
        auto border = makeShared<CTabBorder>(w);
        g_decorations[w.get()] = {bar, border, shadow};
        HyprlandAPI::addWindowDecoration(PHANDLE, w, std::move(shadow));
        HyprlandAPI::addWindowDecoration(PHANDLE, w, std::move(bar));
        HyprlandAPI::addWindowDecoration(PHANDLE, w, std::move(border));
    }

    struct SBarGeometry {
        CBox   box;
        float  tabWidth = 0;
        float  height   = 0;
        float  gap      = 0;
        float  top      = 0;
        size_t count    = 1;
        bool   capped   = false;
    };

    // The tabs have whole device pixels and a gap of one device pixel, where the strip shows through.
    SBarGeometry geometry(const CTabBar* bar, const CBox& assigned) {
        static auto PHEIGHT          = CConfigValue<Config::INTEGER>("group:groupbar:height");
        static auto PINDICATORGAP    = CConfigValue<Config::INTEGER>("group:groupbar:indicator_gap");
        static auto PINDICATORHEIGHT = CConfigValue<Config::INTEGER>("group:groupbar:indicator_height");
        static auto POUTERGAP        = CConfigValue<Config::INTEGER>("group:groupbar:gaps_out");

        const auto   owner = bar->m_owner.lock();
        const auto   mon   = owner ? owner->m_monitor.lock() : nullptr;
        const double scale = mon ? mon->m_scale : 1.0;

        SBarGeometry g;
        g.box               = assigned;
        g.count             = std::max<size_t>(1, bar->m_members.size());
        const double fit    = std::floor((std::round(assigned.w * scale) - (g.count - 1)) / g.count);
        const double capPx  = std::floor(g_config.maxWidth->value() * scale);
        g.capped            = capPx < fit;
        g.tabWidth          = std::max(0.0, std::min(fit, capPx)) / scale;
        g.gap               = 1.0 / scale;
        g.height            = *PHEIGHT;
        g.top               = assigned.y + assigned.h - (*POUTERGAP + *PINDICATORHEIGHT + *PINDICATORGAP + *PHEIGHT);
        return g;
    }

    double tabsEnd(const SBarGeometry& g, size_t count) {
        return g.capped ? g.box.x + count * (g.tabWidth + g.gap) - g.gap : g.box.x + g.box.w;
    }

    // Without the cap, the last tab takes the pixels that the even split leaves over.
    CBox tabBox(const SBarGeometry& g, size_t i) {
        const double x = g.box.x + i * (g.tabWidth + g.gap);
        const double w = !g.capped && i + 1 == g.count ? g.box.x + g.box.w - x : g.tabWidth;
        return {x, g.top, w, g.height};
    }

    CBox closeBox(const CBox& tab) {
        const double size = std::min(18.0, tab.h - 4);
        return {tab.x + tab.w - size - 5, tab.y + (tab.h - size) / 2.0, size, size};
    }

    bool compact(const CBox& tab) {
        return tab.w < g_config.compactWidth->value();
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
    std::optional<double> marqueeOffset(CTabBar* self, double overflow, float scale) {
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

    bool CTabBar::visible() const {
        static auto PDISABLE = CConfigValue<Config::BOOL>("group:groupbar:disable_when_only");
        const auto  w        = m_owner.lock();
        return g_config.enabled->value() && w && w->grouping().group() && !m_members.empty() && (!*PDISABLE || m_members.size() > 1) &&
            w->m_ruleApplicator->decorate().valueOrDefault();
    }

    SDecorationPositioningInfo CTabBar::getPositioningInfo() {
        static auto PHEIGHT          = CConfigValue<Config::INTEGER>("group:groupbar:height");
        static auto PINDICATORGAP    = CConfigValue<Config::INTEGER>("group:groupbar:indicator_gap");
        static auto PINDICATORHEIGHT = CConfigValue<Config::INTEGER>("group:groupbar:indicator_height");
        static auto POUTERGAP        = CConfigValue<Config::INTEGER>("group:groupbar:gaps_out");
        static auto PKEEPUPPERGAP    = CConfigValue<Config::BOOL>("group:groupbar:keep_upper_gap");

        SDecorationPositioningInfo info;
        info.policy   = DECORATION_POSITION_STICKY;
        info.edges    = DECORATION_EDGE_TOP;
        info.priority = 10005;
        info.reserved = true;
        if (visible())
            info.desiredExtents = {Vector2D{0.0, (double)(*POUTERGAP * (1 + *PKEEPUPPERGAP) + *PINDICATORHEIGHT + *PINDICATORGAP + *PHEIGHT)}, Vector2D{0.0, 0.0}};
        return info;
    }

    void CTabBar::reposition() {
        const bool VISIBLE = visible();
        if (!m_lastVisible.has_value() || VISIBLE != *m_lastVisible) {
            m_lastVisible = VISIBLE;
            g_pDecorationPositioner->repositionDeco(this);
        }
    }

    // Hyprland calls this on each join, leave and tab switch, so the bar gets its place in the same frame.
    void CTabBar::updateWindow(PHLWINDOW) {
        const auto w = m_owner.lock();
        m_members.clear();
        if (w && w->grouping().group())
            for (const auto& member : w->grouping().group()->windows())
                m_members.emplace_back(member);
        reposition();
        damageEntire();
    }

    CBox CTabBar::assignedBoxGlobal() const {
        const auto w = m_owner.lock();
        return w ? assignedBoxGlobal(w->presentation().renderPresentation()) : CBox{};
    }

    CBox CTabBar::assignedBoxGlobal(const SWindowRenderPresentation& pres) const {
        CBox box = m_assigned;
        box.translate(g_pDecorationPositioner->getEdgeDefinedPoint(DECORATION_EDGE_TOP, m_owner));
        return box.translate(pres.workspaceOffset);
    }

    void CTabBar::damageEntire() {
        const auto w = m_owner.lock();
        if (!w)
            return;
        auto box = assignedBoxGlobal();
        box.translate(w->presentation().floatingOffset());
        g_pHyprRenderer->damageBox(box);
    }

    void CTabBar::draw(CRenderContext& ctx, PHLMONITOR mon, float const& alpha, const SWindowRenderPresentation& pres) {
        static auto PBLUR = CConfigValue<Config::BOOL>("group:groupbar:blur");

        reposition();
        const auto owner = m_owner.lock();
        if (!visible() || !owner)
            return;

        const auto ASSIGNED = assignedBoxGlobal(pres);
        const auto g        = geometry(this, ASSIGNED);

        const auto  scale    = mon->m_scale;
        const auto  offset   = pres.floatingOffset - mon->m_position;
        const auto  focused  = Desktop::focusState()->window();
        const auto  current  = owner->grouping().group()->current();
        const bool  groupHas = focused && current == focused;
        const auto  R        = globalRounding();
        const auto  count    = m_members.size();
        const bool  blur     = *PBLUR;
        const auto  iconPx   = (int)std::round(g_config.iconSize->value() * scale);
        const auto  padding  = g_config.padding->value();
        const auto& theme    = g_config.iconTheme->value();

        {
            CBox       strip = CBox{g.box.x, g.top, g.box.w, g.height}.translate(offset).scale(scale).round();
            CHyprColor fill  = color(g_config.strip);
            fill.a *= alpha;
            // A tiled bar has only the wallpaper behind it, so the cached wallpaper blur is enough.
            addRect(ctx, strip, fill, Math::SCornerRadii{R.topLeft * scale, R.topRight * scale, 0.F, 0.F}, blur, pres, !owner->isFloating());
        }

        for (size_t i = 0; i < count; ++i) {
            const auto w = m_members[i].lock();
            if (!w)
                continue;

            const bool  isCurrent = w == current;
            const float tabA      = g_drag.active && g_drag.tab == w ? 0.35F : 1.F;
            const float a         = alpha * tabA;
            const bool  isHover   = g_hover.owner == m_owner && g_hover.index == (int)i;
            const bool  isUrgent  = !isCurrent && (w->m_hints & Desktop::View::WINDOW_HINT_URGENT);

            CBox tab = tabBox(g, i);
            CBox px  = tab.copy().translate(offset).scale(scale).round();

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
                const double h  = std::round(g_config.accentHeight->value() * scale);
                CHyprColor   ac = color(g_config.accent);
                ac.a *= a;
                addRect(ctx, CBox{px.x, px.y + px.h - h, px.w, h}, ac, Math::SCornerRadii{0.F}, false, pres);
            }

            const auto icon  = appIcon(w->metadata().appID(), iconPx, theme);
            const bool small = compact(tab);
            double     x     = px.x + (small ? (px.w - iconPx) / 2.0 : padding * scale);
            if (icon) {
                const double iconY = px.y + (px.h - iconPx) / 2.0;
                addTex(ctx, icon, CBox{x, iconY, (double)iconPx, (double)iconPx}, a);
                if (isUrgent) {
                    const double     d    = std::round(7 * scale);
                    const CHyprColor tint = color(g_config.urgent);
                    addRect(ctx, CBox{x + iconPx - d * 0.6, iconY - d * 0.3, d, d}.round(), CHyprColor{(float)tint.r, (float)tint.g, (float)tint.b, a},
                            Math::SCornerRadii{(float)d / 2}, false, pres);
                }
                x += iconPx + 7 * scale;
            } else if (small)
                x = px.x + 4 * scale;

            if (!small || !icon) {
                const bool          showClose = isHover && !small;
                const double        right     = px.x + px.w - (showClose ? 28 : 10) * scale;
                const int           maxW      = std::max(0, (int)(right - x));
                const bool          bright    = isCurrent || isHover || isUrgent;
                const bool          bold      = isCurrent && groupHas;
                auto                text      = maxW > 8 ? titleTexture(w, maxW, scale, bright, bold) : nullptr;
                double              shift     = 0;
                std::optional<CBox> clip;
                if (text && isHover) {
                    const auto full = titleTexture(w, 0, scale, bright, bold, true);
                    if (full && full->m_size.x > maxW)
                        if (const auto marquee = marqueeOffset(this, full->m_size.x - maxW, scale)) {
                            text  = full;
                            shift = *marquee;
                            clip  = CBox{x, px.y, (double)maxW, px.h};
                        }
                }
                if (text)
                    addTex(ctx, text, CBox{x - shift, px.y + std::round((px.h - text->m_size.y) / 2.0), text->m_size.x, text->m_size.y}, a, clip);
            }

            if (isHover && !small) {
                const auto cb = closeBox(tab).translate(offset).scale(scale).round();
                if (g_hover.onClose)
                    addRect(ctx, cb, color(g_config.closeHover), Math::SCornerRadii{(float)(5 * scale)}, false, pres);
                addTex(ctx, closeTexture((int)cb.w, g_config.close->value()), cb, a);
            }
        }
    }

    struct SHit {
        CTabBar* bar     = nullptr;
        int      index   = -1;
        bool     onClose = false;
    };

    SHit hitTest(CTabBar* bar, const Vector2D& pos) {
        if (!bar || !bar->visible())
            return {};
        const auto g = geometry(bar, bar->assignedBoxGlobal());
        for (size_t i = 0; i < bar->m_members.size(); ++i) {
            const auto tab = tabBox(g, i);
            if (!tab.containsPoint(pos))
                continue;
            return {bar, (int)i, !compact(tab) && closeBox(tab).containsPoint(pos)};
        }
        return {};
    }

    void damageOwner(const PHLWINDOWREF& owner) {
        if (const auto bar = barOf(owner.lock()))
            bar->damageEntire();
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
            const auto hit = hitTest(barOf(w), pos);
            if (hit.bar)
                return {w, hit};
        }
        return {nullptr, {}};
    }

    void damageAll() {
        for (const auto& m : State::monitorState()->monitors())
            g_pHyprRenderer->damageMonitor(m);
    }

    bool hyprlandDragging() {
        return g_layoutManager->dragController()->mode() != MBIND_INVALID;
    }

    void onButton(const IPointer::SButtonEvent& e) {
        if (e.button != BTN_LEFT || !g_config.enabled->value())
            return;
        if (e.state != WL_POINTER_BUTTON_STATE_PRESSED) {
            if (g_drag.active)
                damageAll();
            g_drag = {};
            return;
        }
        g_drag                  = {};
        const auto pos          = g_pInputManager->getMouseCoordsInternal();
        const auto [owner, hit] = barAt(pos);
        if (!owner || hit.onClose || hyprlandDragging())
            return;
        const auto tab = tabBox(geometry(hit.bar, hit.bar->assignedBoxGlobal()), hit.index);
        g_drag         = {owner->grouping().group()->fromIndex(hit.index), pos, pos - tab.pos(), tab.size(), false};
    }

    void onDragMove(const Vector2D& pos) {
        if (!g_drag.tab || hyprlandDragging()) {
            if (g_drag.active)
                damageAll();
            g_drag = {};
            return;
        }
        if (!g_drag.active && std::abs(pos.x - g_drag.press.x) + std::abs(pos.y - g_drag.press.y) < DRAG_MIN)
            return;
        g_drag.active = true;
        damageAll();
    }

    enum eZone : uint8_t {
        ZONE_NONE,
        ZONE_CENTER,
        ZONE_TOP,
        ZONE_BOTTOM,
    };

    struct SDrop {
        PHLWINDOW target;
        eZone     zone = ZONE_NONE;
    };

    bool showsTile(const PHLWINDOW& w);
    CBox tileBox(const PHLWINDOW& w);

    // The tile below the point: its bar and the middle join it as a tab, the top and bottom quarter split it.
    SDrop dropAt(const Vector2D& pos, const PHLWINDOW& except) {
        for (const auto& w : Desktop::windowState()->windows()) {
            if (w == except || !showsTile(w) || !tileBox(w).containsPoint(pos))
                continue;
            const CBox win{w->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT), w->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT)};
            if (pos.y < win.y)
                return {w, ZONE_CENTER};
            if (pos.y < win.y + win.h / 4)
                return {w, ZONE_TOP};
            if (pos.y > win.y + win.h * 3 / 4)
                return {w, ZONE_BOTTOM};
            return {w, ZONE_CENTER};
        }
        return {};
    }

    const char* zoneName(eZone zone) {
        return zone == ZONE_TOP ? "top" : zone == ZONE_BOTTOM ? "bottom" : zone == ZONE_CENTER ? "center" : "none";
    }

    bool showsTile(const PHLWINDOW& w) {
        return w->mapped() && !w->isHidden() && !w->isFloating() && w->m_workspace && w->m_workspace->visible() &&
            (!w->grouping().group() || w->grouping().group()->current() == w);
    }

    // The window and the bar above it, in logical global coordinates.
    CBox tileBox(const PHLWINDOW& w) {
        CBox box{w->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT), w->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT)};
        if (const auto bar = barOf(w); bar && bar->visible()) {
            const auto assigned = bar->assignedBoxGlobal();
            box                 = CBox{box.x, assigned.y, box.w, box.y + box.h - assigned.y};
        }
        return box;
    }

    // Own tile: only the bands split it (the middle keeps the tab where it is). Other tile: the middle joins it as a tab.
    void drawZone(CRenderContext& ctx, PHLMONITOR mon, const SDrop& drop, const PHLWINDOW& own) {
        if (!drop.target || drop.target->m_monitor != mon || (drop.zone == ZONE_CENTER && drop.target == own))
            return;
        const auto scale = mon->m_scale;
        const auto R     = globalRounding();
        CBox       box   = drop.zone == ZONE_CENTER ? tileBox(drop.target) :
                                                      CBox{drop.target->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT),
                                                           drop.target->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT)};
        Math::SCornerRadii round{R.topLeft * scale, R.topRight * scale, R.bottomRight * scale, R.bottomLeft * scale};
        if (drop.zone == ZONE_TOP) {
            box.h /= 2;
            round.bottomLeft = round.bottomRight = 0;
        } else if (drop.zone == ZONE_BOTTOM) {
            box.y += box.h / 2;
            box.h /= 2;
            round.topLeft = round.topRight = 0;
        }
        CHyprColor tint = color(g_config.accent);
        tint.a          = 0.16F;
        addRect(ctx, box.translate(-mon->m_position).scale(scale).round(), tint, round, false, {});
    }

    void drawDrag(CRenderContext& ctx, PHLMONITOR mon) {
        const auto tab = g_drag.tab.lock();
        if (!g_drag.active || !tab || !tab->grouping().group())
            return;

        const auto                pos     = g_pInputManager->getMouseCoordsInternal();
        const auto                scale   = mon->m_scale;
        const auto                toPx    = [&](CBox b) { return b.translate(-mon->m_position).scale(scale).round(); };
        const auto                accent  = color(g_config.accent);
        const auto                current = tab->grouping().group()->current();
        SWindowRenderPresentation pres;

        const CBox ghost{pos - g_drag.grab, g_drag.size};
        if (!ghost.intersection(CBox{mon->m_position, mon->m_size}).empty()) {
            const auto px     = toPx(ghost);
            const auto iconPx = (int)std::round(g_config.iconSize->value() * scale);
            const auto pad    = g_config.padding->value() * scale;
            addRect(ctx, px, CHyprColor{0.161F, 0.145F, 0.133F, 0.94F}, Math::SCornerRadii{(float)(6 * scale)}, false, pres);
            const double h = std::round(g_config.accentHeight->value() * scale);
            addRect(ctx, CBox{px.x, px.y + px.h - h, px.w, h}, accent, Math::SCornerRadii{0.F}, false, pres);
            double x = px.x + pad;
            if (const auto icon = appIcon(tab->metadata().appID(), iconPx, g_config.iconTheme->value())) {
                addTex(ctx, icon, CBox{x, px.y + (px.h - iconPx) / 2.0, (double)iconPx, (double)iconPx}, 1.F);
                x += iconPx + 7 * scale;
            }
            const int maxW = std::max(0, (int)(px.x + px.w - 10 * scale - x));
            if (const auto text = maxW > 8 ? titleTexture(tab, maxW, scale, true, true) : nullptr)
                addTex(ctx, text, CBox{x, px.y + std::round((px.h - text->m_size.y) / 2.0), text->m_size.x, text->m_size.y}, 1.F);
        }

        // Where the release puts the tab: a place in its own bar, its own tile, or another tile.
        const auto [owner, hit] = barAt(pos);
        if (owner && owner->grouping().group() == tab->grouping().group()) {
            const auto g    = geometry(hit.bar, hit.bar->assignedBoxGlobal());
            int        from = 0;
            for (size_t i = 0; i < hit.bar->m_members.size(); ++i)
                if (hit.bar->m_members[i] == tab)
                    from = i;
            const auto   target = tabBox(g, hit.index);
            const double x      = hit.index > from ? target.x + target.w - 1 : target.x - 1;
            addRect(ctx, toPx(CBox{x, target.y, 2, target.h}), accent, Math::SCornerRadii{0.F}, false, pres);
        } else
            drawZone(ctx, mon, dropAt(pos, nullptr), current);
    }

    PHLWINDOW draggedWindow() {
        const auto& dc = g_layoutManager->dragController();
        if (dc->mode() != MBIND_MOVE || !dc->draggingTiled() || !dc->target())
            return nullptr;
        return dc->target()->window();
    }

    // Hyprland's drag ends after the layout changed, so the drop is the one seen at the last motion.
    struct {
        PHLWINDOWREF window;
        SDrop        drop;
    } g_pendingDrop;

    void trackDrop() {
        const auto window = draggedWindow();
        if (!window || !g_config.enabled->value())
            return;
        g_pendingDrop = {window, dropAt(g_pInputManager->getMouseCoordsInternal(), window)};
    }

    void onDragEnded() {
        const auto window = g_pendingDrop.window.lock();
        const auto drop   = g_pendingDrop.drop;
        g_pendingDrop     = {};
        if (window && drop.target && drop.zone != ZONE_NONE) {
            const auto call = std::format("tabs_drop(\"{}\", \"{:x}\", \"{:x}\")", zoneName(drop.zone), rc<uintptr_t>(window.get()), rc<uintptr_t>(drop.target.get()));
            g_pEventLoopManager->doLater([call] { HyprlandAPI::invokeHyprctlCommand("eval", call); });
        }
        damageAll();
    }

    int luaDropZone(lua_State* L) {
        const Vector2D pos{luaL_checknumber(L, 1), luaL_checknumber(L, 2)};
        const auto     drop = dropAt(pos, nullptr);
        lua_pushstring(L, zoneName(drop.zone));
        if (drop.target)
            Config::Lua::Objects::CLuaWindow::push(L, drop.target);
        else
            lua_pushnil(L);
        return 2;
    }

    void onMouseMove(const Vector2D& pos) {
        if (!g_config.enabled->value())
            return;
        if (draggedWindow()) {
            trackDrop();
            damageAll();
        }
        if (g_drag.tab)
            onDragMove(pos);
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

    std::pair<int, bool> slotAt(const SBarGeometry& g, const Vector2D& pos) {
        const float rel   = pos.x - g.box.x;
        const int   index = std::min<int>(rel / (g.tabWidth + g.gap), g.count - 1);
        return {index, index + 1 < (int)g.count && rel - (g.tabWidth + g.gap) * index > g.tabWidth};
    }

    bool beginTabDrag(CTabBar* bar, const PHLWINDOW& owner, const Vector2D& pos) {
        const auto group = owner->grouping().group();
        if (group->size() == 1)
            return false;
        const auto [index, onGap] = slotAt(geometry(bar, bar->assignedBoxGlobal()), pos);
        if (onGap)
            return false;
        const auto window = group->fromIndex(index);
        if (!window)
            return false;
        group->remove(window);
        g_layoutManager->dragController()->dragBegin(window->layoutTarget(), MBIND_MOVE);
        if (!Desktop::focusState()->isWindowActive(window))
            Desktop::focusState()->rawWindowFocus(window, Desktop::FOCUS_REASON_CLICK);
        return true;
    }

    bool dropOnBar(CTabBar* bar, const PHLWINDOW& owner, const Vector2D& pos, const PHLWINDOW& dragged) {
        static auto PDRAGINTOGROUP                   = CConfigValue<Config::INTEGER>("group:drag_into_group");
        static auto PMERGEFLOATEDINTOTILEDONGROUPBAR = CConfigValue<Config::BOOL>("group:merge_floated_into_tiled_on_groupbar");
        static auto PMERGEGROUPSONGROUPBAR           = CConfigValue<Config::BOOL>("group:merge_groups_on_groupbar");
        const auto  group                            = owner->grouping().group();
        const bool  floatedIntoTiled                 = !owner->isFloating() && !g_layoutManager->dragController()->draggingTiled();

        if (!dragged || !dragged->grouping().canBeGroupedInto(group) || (*PDRAGINTOGROUP != 1 && *PDRAGINTOGROUP != 2) ||
            (floatedIntoTiled && !*PMERGEFLOATEDINTOTILEDONGROUPBAR) || (!*PMERGEGROUPSONGROUPBAR && dragged->grouping().group()))
            return false;

        const auto  g     = geometry(bar, bar->assignedBoxGlobal());
        const float rel   = std::min<double>(pos.x, tabsEnd(g, bar->m_members.size()) - 1) - g.box.x - g.tabWidth / 2;
        const int   index = (rel < 0 ? -1 : rel / (g.tabWidth + g.gap)) + 1;
        group->add(dragged, index);
        Desktop::focusState()->fullWindowFocus(dragged->windowTarget()->window(), Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
        return true;
    }

    bool clickTab(CTabBar* bar, const PHLWINDOW& owner, const Vector2D& pos, const IPointer::SButtonEvent& e) {
        static auto PMIDDLECLICKCLOSE = CConfigValue<Config::BOOL>("group:groupbar:middle_click_close");
        static auto PFOLLOWMOUSE      = CConfigValue<Config::INTEGER>("input:follow_mouse");
        if (Fullscreen::controller()->getFullscreenModes(owner).internal == Fullscreen::FSMODE_FULLSCREEN)
            return true;

        const auto [index, onGap] = slotAt(geometry(bar, bar->assignedBoxGlobal()), pos);
        const auto group          = owner->grouping().group();

        if (e.button == BTN_MIDDLE) {
            if (!*PMIDDLECLICKCLOSE)
                return true;
            static Vector2D pressedAt;
            if (e.state == WL_POINTER_BUTTON_STATE_PRESSED)
                pressedAt = pos;
            else if (pressedAt == pos)
                if (const auto w = group->fromIndex(index))
                    w->sendClose();
            return true;
        }

        if (e.state != WL_POINTER_BUTTON_STATE_PRESSED)
            return true;

        if (onGap) {
            if (!Desktop::focusState()->isWindowActive(owner))
                Desktop::focusState()->rawWindowFocus(owner, Desktop::FOCUS_REASON_CLICK_DOWN);
            return true;
        }

        const auto window = group->fromIndex(index);
        if (!window)
            return true;
        if (window != owner)
            group->setCurrent(window);
        if (!Desktop::focusState()->isWindowActive(window) && *PFOLLOWMOUSE != 3)
            Desktop::focusState()->rawWindowFocus(window, Desktop::FOCUS_REASON_CLICK_DOWN);
        if (window->isFloating())
            Desktop::windowState()->raise(window);
        return true;
    }

    // A tab switch is instant: Hyprland moves the focus only when the group had it, so tabs.lua sees no window.active
    // and cannot set no_anim, and a scroll on the bar of another group faded the tabs.
    bool scrollTabs(const PHLWINDOW& owner, const IPointer::SAxisEvent& e) {
        static auto PSCROLLING = CConfigValue<Config::BOOL>("group:groupbar:scrolling");
        const auto  group      = owner->grouping().group();
        if (!*PSCROLLING || !group)
            return false;
        group->moveCurrent(e.delta > 0);
        for (const auto& ref : group->windows()) {
            const auto w = ref.lock();
            if (!w)
                continue;
            w->presentation().alpha(Desktop::View::WINDOW_ALPHA_LAYOUT)->warp();
            w->positionAnimation()->warp();
            w->sizeAnimation()->warp();
        }
        return true;
    }

    bool CTabBar::onInputOnDeco(const eInputType type, const Vector2D& pos, std::any data) {
        const auto owner = m_owner.lock();
        if (!owner || !owner->grouping().group() || !visible())
            return false;

        if (type == INPUT_TYPE_AXIS)
            return scrollTabs(owner, std::any_cast<const IPointer::SAxisEvent>(data));
        if (type == INPUT_TYPE_DRAG_END)
            return dropOnBar(this, owner, pos, std::any_cast<PHLWINDOW>(data));

        const auto pastTabs = pos.x > tabsEnd(geometry(this, assignedBoxGlobal()), m_members.size());
        const auto hit      = hitTest(this, pos);

        if (type == INPUT_TYPE_DRAG_START) {
            if (pastTabs)
                return false;
            return hit.onClose ? true : beginTabDrag(this, owner, pos);
        }
        if (type != INPUT_TYPE_BUTTON)
            return false;

        if (pastTabs) {
            if (!Desktop::focusState()->isWindowActive(owner))
                Desktop::focusState()->rawWindowFocus(owner, Desktop::FOCUS_REASON_CLICK);
            return true;
        }

        const auto e = std::any_cast<const IPointer::SButtonEvent&>(data);
        if (e.button == BTN_RIGHT && hit.bar) {
            const auto tab = owner->grouping().group()->fromIndex(hit.index);
            if (tab && e.state == WL_POINTER_BUTTON_STATE_PRESSED)
                IPC::Socket2::sock()->postEvent({"tabbar-menu",
                                                 std::format("{:x},{},{},{},{}", rc<uintptr_t>(tab.get()), hit.index + 1, m_members.size(), (int)std::round(pos.x),
                                                             (int)std::round(pos.y))});
            return true;
        }
        if (e.button != BTN_LEFT)
            return clickTab(this, owner, pos, e);

        if (e.state == WL_POINTER_BUTTON_STATE_PRESSED) {
            if (!hit.onClose) {
                g_pressedClose.reset();
                return clickTab(this, owner, pos, e);
            }
            g_pressedClose = owner->grouping().group()->fromIndex(hit.index);
            return true;
        }

        if (g_pressedClose) {
            const auto target = g_pressedClose.lock();
            g_pressedClose.reset();
            if (hit.onClose && target && target == owner->grouping().group()->fromIndex(hit.index))
                target->sendClose();
            return true;
        }
        return clickTab(this, owner, pos, e);
    }

    CHyprBorderDecoration* CTabBorder::hyprlandBorder() const {
        const auto w = m_owner.lock();
        return w ? dc<CHyprBorderDecoration*>(w->presentation().decoration(DECORATION_BORDER).get()) : nullptr;
    }

    // Hyprland's border of a window with a bar is transparent; this one has the same size and colours, but round
    // top corners around the bar.
    void CTabBorder::draw(CRenderContext& ctx, PHLMONITOR mon, float const& a, const SWindowRenderPresentation& pres) {
        const auto w      = m_owner.lock();
        const auto border = hyprlandBorder();
        if (!w || !border || !hasBar(w))
            return;
        const auto size = border->renderBorderSize();
        if (size == 0)
            return;

        CBox box = g_pDecorationPositioner->getWindowDecorationBox(border)
                       .translate(pres.workspaceOffset - mon->m_position + pres.floatingOffset)
                       .expand(-border->borderExtent())
                       .scale(mon->m_scale)
                       .round();
        if (box.width < 1 || box.height < 1)
            return;

        const auto gradient   = m_gradient.renderState();
        const auto base       = globalRounding();
        const auto power      = w->presentation().roundingPower();
        const auto correction = size * (M_SQRT2 - 1) * std::max(2.0 - power, 0.0);

        CBorderPassElement::SBorderData data;
        data.box           = box;
        data.grad1         = gradient.current;
        data.round         = base * mon->m_scale;
        data.outerRound    = base.expanded(size - correction) * mon->m_scale;
        data.roundingPower = power;
        data.a             = a;
        data.borderSize    = size;
        data.window        = w;
        if (gradient.transitioning) {
            data.hasGrad2 = true;
            data.grad1    = gradient.previous;
            data.grad2    = gradient.current;
            data.lerp     = gradient.progress;
        }
        g_pHyprRenderer->addPassElement(ctx, makeUnique<CBorderPassElement>(data));
    }

    void CTabBorder::damageEntire() {
        const auto w      = m_owner.lock();
        const auto border = hyprlandBorder();
        if (!validMapped(w) || !border || !hasBar(w) || Fullscreen::controller()->isPresentedFullscreen(w, Fullscreen::FSMODE_FULLSCREEN))
            return;

        const auto box = g_pDecorationPositioner->getWindowDecorationBox(border).translate(w->presentation().renderPresentation().workspaceOffset +
                                                                                           w->presentation().floatingOffset());
        if (box.w <= 0 || box.h <= 0)
            return;
        const auto ring = border->borderExtent() + 1 + globalRounding().max();
        CRegion    region(box);
        region.subtract(box.copy().expand(-ring));
        region.expand(2);
        g_pHyprRenderer->damageRegion(region);
    }

    void CTabBorder::updateState() {
        static auto PGROUPACTIVECOL         = CConfigValue<Config::IComplexConfigValue>("group:col.border_active");
        static auto PGROUPINACTIVECOL       = CConfigValue<Config::IComplexConfigValue>("group:col.border_inactive");
        static auto PGROUPACTIVELOCKEDCOL   = CConfigValue<Config::IComplexConfigValue>("group:col.border_locked_active");
        static auto PGROUPINACTIVELOCKEDCOL = CConfigValue<Config::IComplexConfigValue>("group:col.border_locked_inactive");

        const auto  w = m_owner.lock();
        if (!w)
            return;

        const auto group  = w->grouping().group();
        const bool locked = (group && group->locked()) || Desktop::windowState()->groupsLocked();
        if (w == Desktop::focusState()->window()) {
            const auto* col = sc<Config::CGradientValueData*>((locked ? PGROUPACTIVELOCKEDCOL : PGROUPACTIVECOL).ptr());
            m_gradient.setTarget(withoutOwnProp(w->m_ruleApplicator->activeBorderColor()).valueOr(*col));
        } else {
            const auto* col = sc<Config::CGradientValueData*>((locked ? PGROUPINACTIVELOCKEDCOL : PGROUPINACTIVECOL).ptr());
            m_gradient.setTarget(withoutOwnProp(w->m_ruleApplicator->inactiveBorderColor()).valueOr(*col));
        }
    }

    SDecorationPositioningInfo CTabShadow::getPositioningInfo() {
        SDecorationPositioningInfo info;
        info.policy         = DECORATION_POSITION_ABSOLUTE;
        info.desiredExtents = m_extents;
        info.edges          = DECORATION_EDGE_BOTTOM | DECORATION_EDGE_LEFT | DECORATION_EDGE_RIGHT | DECORATION_EDGE_TOP;
        m_reportedExtents   = m_extents;
        return info;
    }

    void CTabShadow::updateWindow(PHLWINDOW) {
        const auto w = m_owner.lock();
        if (!w)
            return;
        m_lastPos          = w->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
        m_lastSize         = w->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
        m_lastBoxWithDecos = g_pDecorationPositioner->getBoxWithIncludedDecos(w);
    }

    void CTabShadow::updateState() {
        static auto PSHADOWCOL         = CConfigValue<Config::IComplexConfigValue>("decoration:shadow:color");
        static auto PSHADOWCOLINACTIVE = CConfigValue<Config::IComplexConfigValue>("decoration:shadow:color_inactive");

        const auto  w = m_owner.lock();
        if (!w)
            return;
        const auto traits = w->backend().traits();
        if (traits.overrideRedirect || traits.suggestsNoBorder) {
            m_gradient.setTarget(Config::CGradientValueData{CHyprColor(0, 0, 0, 0)}, false);
            return;
        }
        auto* const active   = sc<Config::CGradientValueData*>(PSHADOWCOL.ptr());
        auto* const inactive = sc<Config::CGradientValueData*>(PSHADOWCOLINACTIVE.ptr());
        if (w == Desktop::focusState()->window())
            m_gradient.setTarget(*active);
        else
            m_gradient.setTarget(PSHADOWCOLINACTIVE.setByUser() ? *inactive : *active);
    }

    bool CTabShadow::canRender() const {
        static auto PSHADOWS = CConfigValue<Config::BOOL>("decoration:shadow:enabled");
        const auto  w        = m_owner.lock();
        if (!*PSHADOWS || !g_config.enabled->value() || !validMapped(w))
            return false;

        const auto hasAlpha = [](const Config::CGradientValueData& g) { return std::ranges::any_of(g.m_colors, [](const auto& c) { return c.a > 0.001F; }); };
        const auto gradient = m_gradient.renderState();
        if (!hasAlpha(gradient.current) && !(gradient.transitioning && hasAlpha(gradient.previous)))
            return false;

        return w->m_ruleApplicator->decorate().valueOrDefault() && !withoutOwnProp(w->m_ruleApplicator->noShadow()).valueOr(false);
    }

    void CTabShadow::draw(CRenderContext& ctx, PHLMONITOR, float const& a, const SWindowRenderPresentation& pres) {
        const auto self = dynamicPointerCast<CTabShadow>(this->self());
        if (self)
            g_pHyprRenderer->addPassElement(ctx, makeUnique<CTabShadowPass>(self, a, pres));
    }

    // Hyprland's shadow, with the outline of a window with a bar (round on all four corners) and a smaller, lighter
    // shadow on tiled windows: they lie closer to the wallpaper than floating ones.
    void CTabShadow::render(CRenderContext& ctx, PHLMONITOR mon, float a, const SWindowRenderPresentation& pres) {
        static auto PRANGE  = CConfigValue<Config::INTEGER>("decoration:shadow:range");
        static auto PSCALE  = CConfigValue<Config::FLOAT>("decoration:shadow:scale");
        static auto POFFSET = CConfigValue<Config::VEC2>("decoration:shadow:offset");
        static auto PSHARP  = CConfigValue<Config::BOOL>("decoration:shadow:sharp");

        const auto  w = m_owner.lock();
        if (!mon || !w || !canRender())
            return;

        const bool  tiled  = !w->isFloating();
        const float factor = tiled ? std::clamp(g_config.tiledShadow->value(), 0.F, 1.F) : 1.F;
        if (tiled)
            a *= std::clamp(g_config.tiledShadowAlpha->value(), 0.F, 1.F);

        const auto border     = w->presentation().borderSize();
        const auto base       = hasBar(w) ? globalRounding() : Math::SCornerRadii{w->presentation().rounding()};
        const auto power      = w->presentation().roundingPower();
        const auto correction = border * (M_SQRT2 - 1) * std::max(2.0 - power, 0.0);
        const auto rounding   = base.expanded(border - correction);

        CBox fullBox = m_lastBoxWithDecos;
        fullBox.translate(-mon->m_position + pres.workspaceOffset);
        fullBox.x -= *PRANGE;
        fullBox.y -= *PRANGE;
        fullBox.w += 2 * *PRANGE;
        fullBox.h += 2 * *PRANGE;
        fullBox.scaleFromCenter(std::clamp(*PSCALE, 0.F, 1.F)).translate({(*POFFSET).x, (*POFFSET).y});

        updateWindow(w);
        m_lastPos += pres.workspaceOffset;
        m_extents = {
            .topLeft     = {m_lastPos.x - fullBox.x - mon->m_position.x + 2, m_lastPos.y - fullBox.y - mon->m_position.y + 2},
            .bottomRight = {fullBox.x + fullBox.width + mon->m_position.x - m_lastPos.x - m_lastSize.x + 2,
                            fullBox.y + fullBox.height + mon->m_position.y - m_lastPos.y - m_lastSize.y + 2},
        };

        fullBox.translate(pres.floatingOffset);
        if (fullBox.width < 1 || fullBox.height < 1)
            return;
        fullBox.scale(mon->m_scale).round();

        int range = *PRANGE;
        if (factor < 1.F) {
            const double   shrink = (1.0 - factor) * mon->m_scale;
            const double   inset  = *PRANGE * std::clamp(*PSCALE, 0.F, 1.F) * shrink;
            const Vector2D center = fullBox.middle() - Vector2D{(*POFFSET).x, (*POFFSET).y} * shrink;
            const Vector2D size   = fullBox.size() - Vector2D{inset, inset} * 2;
            if (size.x >= 1 && size.y >= 1) {
                fullBox = CBox{center - size / 2, size}.round();
                range   = std::max(1, (int)std::round(range * factor));
            }
        }

        ctx.m_data.currentWindow = w;
        g_pHyprRenderer->disableScissor();
        g_pHyprRenderer->blend(true);

        const auto gradient = m_gradient.renderState();
        const auto round    = rounding * mon->m_scale;
        if (*PSHARP) {
            const auto& from = gradient.transitioning ? gradient.previous : gradient.current;
            CHyprColor  flat = from.m_colors.empty() ? CHyprColor(0, 0, 0, 0) : from.m_colors[0];
            if (gradient.transitioning && !gradient.current.m_colors.empty()) {
                const auto to = gradient.current.m_colors[0];
                const auto t  = gradient.progress;
                flat          = CHyprColor(flat.r + (to.r - flat.r) * t, flat.g + (to.g - flat.g) * t, flat.b + (to.b - flat.b) * t, flat.a + (to.a - flat.a) * t);
            }
            flat.a *= a;
            g_pHyprRenderer->draw(ctx, CRectPassElement::SRectData{.box = fullBox, .color = flat, .round = round, .roundingPower = power}, fullBox);
        } else if (gradient.transitioning)
            g_pHyprRenderer->drawShadow(ctx, fullBox, round, power, range * mon->m_scale, gradient.previous, gradient.current, gradient.progress, a, pres);
        else
            g_pHyprRenderer->drawShadow(ctx, fullBox, round, power, range * mon->m_scale, gradient.current, a, pres);

        if (m_extents != m_reportedExtents)
            g_pDecorationPositioner->repositionDeco(this);
        ctx.m_data.currentWindow.reset();
    }

    void CTabShadow::damageEntire() {
        static auto PSHADOWS = CConfigValue<Config::BOOL>("decoration:shadow:enabled");
        const auto  w        = m_owner.lock();
        if (!*PSHADOWS || !w)
            return;

        const auto pos  = w->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
        const auto size = w->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
        CBox       box  = {pos.x - m_extents.topLeft.x, pos.y - m_extents.topLeft.y, size.x + m_extents.topLeft.x + m_extents.bottomRight.x,
                           size.y + m_extents.topLeft.y + m_extents.bottomRight.y};

        const auto workspace = w->presentation().workspace();
        if (workspace && workspace->m_renderOffset->isBeingAnimated() && !(w->m_state & Desktop::View::WINDOW_STATE_PINNED))
            box.translate(workspace->m_renderOffset->value());
        box.translate(w->presentation().floatingOffset());

        CRegion region(box);
        for (const auto& m : State::monitorState()->monitors())
            if (!g_pHyprRenderer->shouldRenderWindow(w, m))
                region.subtract(CRegion({m->m_position, m->m_size}));
        g_pHyprRenderer->damageRegion(region);
    }

    std::string squareTop() {
        const auto r = globalRounding();
        return std::format("0 0 {} {}", (int)r.bottomRight, (int)r.bottomLeft);
    }

    // Hyprland's own shadow goes off on every window (the plugin draws it); a window with a bar also gets a
    // transparent Hyprland border (the plugin draws it) and square top corners under the bar.
    void apply(const PHLWINDOW& w, bool shadow, const std::string& rounding) {
        auto&      rules  = *w->m_ruleApplicator;
        const bool border = !rounding.empty();
        const Config::CGradientValueData none{CHyprColor(sc<uint64_t>(0))};

        if (shadow)
            rules.noShadow().set(true, PRIORITY_SET_PROP);
        else
            rules.noShadow().unset(PRIORITY_SET_PROP);

        if (border) {
            rules.activeBorderColor().set(none, PRIORITY_SET_PROP);
            rules.inactiveBorderColor().set(none, PRIORITY_SET_PROP);
        } else {
            rules.activeBorderColor().unset(PRIORITY_SET_PROP);
            rules.inactiveBorderColor().unset(PRIORITY_SET_PROP);
        }

        (void)Config::Actions::setProp("rounding", border ? rounding : "unset", w);
    }

    void syncWindows() {
        const bool enabled = g_config.enabled->value();
        const auto value   = squareTop();

        for (auto it = g_applied.begin(); it != g_applied.end();) {
            if (!it->second.window)
                it = g_applied.erase(it);
            else
                ++it;
        }

        for (const auto& w : Desktop::windowState()->windows()) {
            if (!w->mapped())
                continue;
            attach(w);
            const bool shadow   = enabled;
            const auto rounding = enabled && hasBar(w) ? value : std::string{};
            const auto it       = g_applied.find(w.get());
            const bool known    = it != g_applied.end();

            if (known && it->second.shadow == shadow && it->second.rounding == rounding)
                continue;
            if (!known && !shadow && rounding.empty())
                continue;

            apply(w, shadow, rounding);
            if (!shadow && rounding.empty())
                g_applied.erase(w.get());
            else
                g_applied[w.get()] = {w, shadow, rounding};
        }
    }

    void forgetWindow(Desktop::View::CWindow* w) {
        g_applied.erase(w);
        g_decorations.erase(w);
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

    CHyprSignalListener g_dragMotion, g_dragEnded;
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
    g_config.enabled          = makeShared<CBoolValue>("plugin:tabbar:enabled", "Draw the group tab bars", true);
    g_config.strip            = makeShared<CColorValue>("plugin:tabbar:col.strip", "Bar behind the tabs", 0xa6292522);
    g_config.focused          = makeShared<CColorValue>("plugin:tabbar:col.focused", "Current tab of the focused group", 0x29ece1d7);
    g_config.current          = makeShared<CColorValue>("plugin:tabbar:col.current", "Current tab of another group", 0x12ece1d7);
    g_config.inactive         = makeShared<CColorValue>("plugin:tabbar:col.inactive", "Other tabs", 0x00000000);
    g_config.hover            = makeShared<CColorValue>("plugin:tabbar:col.hover", "Tab under the pointer", 0x1fffffff);
    g_config.urgent           = makeShared<CColorValue>("plugin:tabbar:col.urgent", "Tint of a tab that asks for attention", 0x40d47766);
    g_config.accent           = makeShared<CColorValue>("plugin:tabbar:col.accent", "Line under the current tab of the focused group", 0xffe49b5d);
    g_config.close            = makeShared<CColorValue>("plugin:tabbar:col.close", "Close button", 0xffece1d7);
    g_config.closeHover       = makeShared<CColorValue>("plugin:tabbar:col.close_hover", "Behind the close button under the pointer", 0x29ece1d7);
    g_config.accentHeight     = makeShared<CIntValue>("plugin:tabbar:accent_height", "Height of the accent line, 0 = none", 2);
    g_config.iconSize         = makeShared<CIntValue>("plugin:tabbar:icon_size", "App icon size", 16);
    g_config.padding          = makeShared<CIntValue>("plugin:tabbar:padding", "Space left of the icon", 10);
    g_config.compactWidth     = makeShared<CIntValue>("plugin:tabbar:compact_width", "Below this tab width only the icon shows", 90);
    g_config.maxWidth         = makeShared<CIntValue>("plugin:tabbar:max_width", "Largest tab width", 240);
    g_config.tiledShadow      = makeShared<CFloatValue>("plugin:tabbar:tiled_shadow", "Shadow of tiled windows relative to floating ones (range and offset), 1 = the same", 0.35);
    g_config.tiledShadowAlpha = makeShared<CFloatValue>("plugin:tabbar:tiled_shadow_alpha", "Opacity of the shadow of tiled windows relative to floating ones", 0.7);
    g_config.iconTheme        = makeShared<CStringValue>("plugin:tabbar:icon_theme", "Icon theme, hicolor is the fallback", "Adwaita");

    for (const auto& v : std::initializer_list<SP<IValue>>{g_config.enabled, g_config.strip, g_config.focused, g_config.current, g_config.inactive, g_config.hover, g_config.urgent,
                                                           g_config.accent, g_config.close, g_config.closeHover, g_config.accentHeight, g_config.iconSize, g_config.padding,
                                                           g_config.compactWidth, g_config.maxWidth, g_config.iconTheme, g_config.tiledShadow, g_config.tiledShadowAlpha})
        HyprlandAPI::addConfigValueV2(PHANDLE, v);

    static auto P0 = Event::bus()->m_events.window.open.listen([](PHLWINDOW w) { attach(w); });
    static auto P1 = Event::bus()->m_events.input.mouse.move.listen([](Vector2D pos, Event::SCallbackInfo&) { onMouseMove(pos); });
    static auto P2 = Event::bus()->m_events.render.pre.listen([](PHLMONITOR) { syncWindows(); });
    static auto P7 = Event::bus()->m_events.input.mouse.button.listen([](IPointer::SButtonEvent e, Event::SCallbackInfo&) { onButton(e); });
    static auto P8 = Event::bus()->m_events.render.stage.listen([](Event::SRenderStageEvent e) {
        if (e.stage != RENDER_POST_WINDOWS || !e.context)
            return;
        if (g_drag.active)
            drawDrag(e.context->get(), e.monitor);
        else if (const auto window = draggedWindow())
            drawZone(e.context->get(), e.monitor, dropAt(g_pInputManager->getMouseCoordsInternal(), window), nullptr);
    });
    static auto P3 = Event::bus()->m_events.window.destroy.listen([](PHLWINDOWREF w) { forgetWindow(w.get()); });
    static auto P4 = Event::bus()->m_events.window.urgent.listen([](PHLWINDOW w) { damageGroupOf(w); });
    static auto P5 = Event::bus()->m_events.window.title.listen([](PHLWINDOW w) { damageGroupOf(w); });
    static auto P6 = Event::bus()->m_events.config.reloaded.listen([] {
        clearIconCache();
        g_closeTex.clear();
        g_text.clear();
    });

    g_dragMotion = g_layoutManager->dragController()->m_events.motion.listen([] { trackDrop(); });
    g_dragEnded  = g_layoutManager->dragController()->m_events.ended.listen([] { onDragEnded(); });

    g_marqueeTimer = makeShared<CEventLoopTimer>(std::nullopt, [](SP<CEventLoopTimer>, void*) { damageOwner(g_hover.owner); }, nullptr);
    g_pEventLoopManager->addTimer(g_marqueeTimer);

    HyprlandAPI::addLuaFunction(PHANDLE, "tabbar", "tab_at", luaTabAt);
    HyprlandAPI::addLuaFunction(PHANDLE, "tabbar", "drop_zone", luaDropZone);

    for (const auto& w : Desktop::windowState()->windows())
        attach(w);

    HyprlandAPI::reloadConfig();

    return {"tabbar", "Group tab bars with app icons, hover, close button and per-corner rounding", "hadi", "0.2"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_dragMotion.reset();
    g_dragEnded.reset();
    if (g_marqueeTimer) {
        g_pEventLoopManager->removeTimer(g_marqueeTimer);
        g_marqueeTimer.reset();
    }
    for (auto& [ptr, entry] : g_applied)
        if (const auto w = entry.window.lock())
            apply(w, false, "");
    g_applied.clear();
    g_decorations.clear();
    g_text.clear();
    g_closeTex.clear();
    clearIconCache();
}
