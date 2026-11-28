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
#include <hyprland/src/config/values/types/BoolValue.hpp>
#include <hyprland/src/config/values/types/ColorValue.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/decorations/IHyprWindowDecoration.hpp>
#include <hyprland/src/render/decorations/DecorationPositioner.hpp>
#include <hyprland/src/render/decorations/AnimatedDecorationGradient.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/supplementary/DragController.hpp>
#include <hyprland/src/layout/target/WindowTarget.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/workspace/HLWorkspace.hpp>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

#include <cmath>
#include <cstdlib>
#include <format>
#include <unordered_map>

using namespace Render;
using Desktop::Types::PRIORITY_SET_PROP;

inline HANDLE PHANDLE = nullptr;

namespace {
    struct {
        SP<Config::Values::CBoolValue>  enabled;
        SP<Config::Values::CColorValue> accent;
        SP<Config::Values::CFloatValue> tiledShadow, tiledShadowAlpha;
    } g_config;

    CHyprColor color(const SP<Config::Values::CColorValue>& v) {
        return CHyprColor{sc<uint64_t>(v->value())};
    }

    Math::SCornerRadii globalRounding() {
        static auto PROUNDING = CConfigValue<Config::IComplexConfigValue>("decoration:rounding");
        return Math::SCornerRadii{*sc<Config::CCornersData*>(PROUNDING.ptr())};
    }

    void damageAll() {
        for (const auto& m : State::monitorState()->monitors())
            g_pHyprRenderer->damageMonitor(m);
    }

    // The plugin sets this prop itself; its own shadow follows the user's rules only.
    template <typename T>
    Desktop::Types::COverridableVar<T> withoutOwnProp(const Desktop::Types::COverridableVar<T>& var) {
        auto copy = var;
        copy.unset(PRIORITY_SET_PROP);
        return copy;
    }

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

    // Hyprland's shadow, but smaller and lighter on tiled windows: they lie closer to the wallpaper than floating ones.
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
        const auto base       = Math::SCornerRadii{w->presentation().rounding()};
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

    std::unordered_map<Desktop::View::CWindow*, WP<CTabShadow>> g_shadows;
    // The windows whose Hyprland shadow the plugin turned off: its own shadow replaces it.
    std::unordered_map<Desktop::View::CWindow*, PHLWINDOWREF> g_applied;

    void attach(const PHLWINDOW& w) {
        if (!validMapped(w) || g_shadows.contains(w.get()))
            return;
        auto shadow        = makeShared<CTabShadow>(w);
        g_shadows[w.get()] = shadow;
        HyprlandAPI::addWindowDecoration(PHANDLE, w, std::move(shadow));
    }

    void setOwnShadow(const PHLWINDOW& w, bool own) {
        if (own)
            w->m_ruleApplicator->noShadow().set(true, PRIORITY_SET_PROP);
        else
            w->m_ruleApplicator->noShadow().unset(PRIORITY_SET_PROP);
    }

    void syncWindows() {
        const bool enabled = g_config.enabled->value();
        std::erase_if(g_applied, [](const auto& entry) { return !entry.second; });
        for (const auto& w : Desktop::windowState()->windows()) {
            if (!w->mapped())
                continue;
            attach(w);
            if (enabled == g_applied.contains(w.get()))
                continue;
            setOwnShadow(w, enabled);
            if (enabled)
                g_applied[w.get()] = w;
            else
                g_applied.erase(w.get());
        }
    }

    void forgetWindow(Desktop::View::CWindow* w) {
        g_applied.erase(w);
        g_shadows.erase(w);
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

    bool showsTile(const PHLWINDOW& w) {
        return w->mapped() && !w->isHidden() && !w->isFloating() && w->m_workspace && w->m_workspace->visible() &&
            (!w->grouping().group() || w->grouping().group()->current() == w);
    }

    CBox tileBox(const PHLWINDOW& w) {
        return {w->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT), w->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT)};
    }

    // The tile below the point: the middle joins it as a tab, the top and bottom quarter split it.
    SDrop dropAt(const Vector2D& pos, const PHLWINDOW& except) {
        for (const auto& w : Desktop::windowState()->windows()) {
            if (w == except || !showsTile(w))
                continue;
            const auto win = tileBox(w);
            if (!win.containsPoint(pos))
                continue;
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

    void drawZone(CRenderContext& ctx, PHLMONITOR mon, const SDrop& drop) {
        if (!drop.target || drop.target->m_monitor != mon)
            return;
        const auto         scale = mon->m_scale;
        const auto         R     = globalRounding();
        CBox               box   = tileBox(drop.target);
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

        CRectPassElement::SRectData data;
        data.box   = box.translate(-mon->m_position).scale(scale).round();
        data.color = tint;
        data.round = round;
        g_pHyprRenderer->addPassElement(ctx, makeUnique<CRectPassElement>(data));
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

    void onMouseMove() {
        if (g_config.enabled->value() && draggedWindow()) {
            trackDrop();
            damageAll();
        }
    }

    // True once the window's app has committed a frame at the size that Hyprland last configured: a window that
    // shows before that shows its previous frame. tabs.lua waits for it before an app takes a new tab's place.
    int luaReady(lua_State* L) {
        const auto address = std::strtoull(luaL_checkstring(L, 1), nullptr, 16);
        for (const auto& w : Desktop::windowState()->windows()) {
            if (rc<uintptr_t>(w.get()) != address)
                continue;
            const auto& backend = w->backend();
            const auto  drawn   = backend.geometry().box.size();
            const auto  asked   = backend.reportedSize();
            lua_pushboolean(L, w->mapped() && asked.x > 0 && std::abs(drawn.x - asked.x) < 1 && std::abs(drawn.y - asked.y) < 1);
            return 1;
        }
        lua_pushboolean(L, false);
        return 1;
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
    g_config.enabled          = makeShared<CBoolValue>("plugin:tabbar:enabled", "Draw the window shadows (smaller on tiled windows) and the drop zones of a tiled window drag", true);
    g_config.accent           = makeShared<CColorValue>("plugin:tabbar:col.accent", "Colour of a drop zone", 0xffe49b5d);
    g_config.tiledShadow      = makeShared<CFloatValue>("plugin:tabbar:tiled_shadow", "Shadow of tiled windows relative to floating ones (range and offset), 1 = the same", 0.35);
    g_config.tiledShadowAlpha = makeShared<CFloatValue>("plugin:tabbar:tiled_shadow_alpha", "Opacity of the shadow of tiled windows relative to floating ones", 0.7);
    for (const auto& v : std::initializer_list<SP<IValue>>{g_config.enabled, g_config.accent, g_config.tiledShadow, g_config.tiledShadowAlpha})
        HyprlandAPI::addConfigValueV2(PHANDLE, v);

    static auto P0 = Event::bus()->m_events.window.open.listen([](PHLWINDOW w) { attach(w); });
    static auto P2 = Event::bus()->m_events.render.pre.listen([](PHLMONITOR) { syncWindows(); });
    static auto P3 = Event::bus()->m_events.window.destroy.listen([](PHLWINDOWREF w) { forgetWindow(w.get()); });
    static auto P4 = Event::bus()->m_events.input.mouse.move.listen([](Vector2D, Event::SCallbackInfo&) { onMouseMove(); });
    static auto P1 = Event::bus()->m_events.render.stage.listen([](Event::SRenderStageEvent e) {
        if (e.stage != RENDER_POST_WINDOWS || !e.context)
            return;
        if (const auto window = draggedWindow(); window && g_config.enabled->value())
            drawZone(e.context->get(), e.monitor, dropAt(g_pInputManager->getMouseCoordsInternal(), window));
    });

    g_dragMotion = g_layoutManager->dragController()->m_events.motion.listen([] { trackDrop(); });
    g_dragEnded  = g_layoutManager->dragController()->m_events.ended.listen([] { onDragEnded(); });

    HyprlandAPI::addLuaFunction(PHANDLE, "tabbar", "ready", luaReady);

    for (const auto& w : Desktop::windowState()->windows())
        attach(w);

    HyprlandAPI::reloadConfig();

    return {"tabbar", "Window shadows (smaller on tiled windows), drop zones for tiled window drags, and the Lua helper ready",
            "hadi", "0.3"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_dragMotion.reset();
    g_dragEnded.reset();
    for (auto& [ptr, window] : g_applied)
        if (const auto w = window.lock())
            setOwnShadow(w, false);
    g_applied.clear();
    g_shadows.clear();
}
