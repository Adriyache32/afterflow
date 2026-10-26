#define WLR_USE_UNSTABLE

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/pass/Pass.hpp>
#include <hyprland/src/render/pass/SurfacePassElement.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/desktop/rule/windowRule/WindowRuleApplicator.hpp>
#include <hyprland/src/managers/screenshare/ScreenshareManager.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>

#include <algorithm>
#include <format>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

// Afterflow capture exclusion
//
// Hyprland's built-in no_screen_share rule draws an opaque black rectangle over
// the window in every capture.  That is easy to implement, but it means the
// window is also unusable to the person sitting in front of the monitor.
//
// This plugin keeps the same window rule as the opt-in signal, but changes the
// rendering path:
//
//   * while a monitor/region screencopy session is active, the marked windows
//     are skipped by the normal render pass;
//   * the mirror framebuffer (the one screencopy reads) is copied before those
//     windows are added back, so the capture never contains them;
//   * the windows are then rendered into the normal output framebuffer, so the
//     local user still sees the terminal and its contents.
//
// The result is real per-window capture exclusion on the local output, not a
// black box.  Window sharing of a no_screen_share window is still rejected by
// Hyprland's own window-share check.

namespace {

    HANDLE g_pluginHandle = nullptr;

    CFunctionHook* g_shouldRenderWindowHook  = nullptr;
    CFunctionHook* g_renderWindowHook        = nullptr;
    CFunctionHook* g_saveMirrorHook          = nullptr;
    CFunctionHook* g_renderMonitorHook       = nullptr;
    CFunctionHook* g_screenshareRenderHook   = nullptr;

    // Set while the compositor is inside IHyprRenderer::renderMonitor().
    bool g_inMainRender = false;

    // Set while Screenshare::CScreenshareFrame::renderMonitor() is drawing a
    // capture frame.  The black-box loop lives in that function, which can run
    // from the output commit path and therefore also while g_inMainRender is
    // still true.  A separate flag is needed to recognise it reliably.
    bool g_inScreenshareRender = false;

    // Set while we are re-adding the skipped windows to the output framebuffer.
    bool g_reAdding = false;

    // Windows skipped in the current normal frame.  The list is populated by the
    // renderWindow hook and consumed by the saveBufferForMirror hook.
    std::vector<PHLWINDOW> g_skippedWindows;

    // Per-monitor session count from the previous frame, used to force a full
    // damage frame when a capture starts so a previously valid mirror cannot
    // contain a stale copy of the terminal.
    std::unordered_set<std::string> g_capturedMonitors;

    bool isNoScreenShare(PHLWINDOW window) {
        return window && window->m_ruleApplicator && window->m_ruleApplicator->noScreenShare().valueOrDefault();
    }

    bool isCaptured(PHLMONITOR monitor) {
        return monitor && Screenshare::mgr() && Screenshare::mgr()->isOutputBeingSSd(monitor);
    }

    using origShouldRenderWindow    = bool (*)(Render::IHyprRenderer*, PHLWINDOW, PHLMONITOR);
    using origRenderWindow          = void (*)(Render::IHyprRenderer*, PHLWINDOW, PHLMONITOR, const Time::steady_tp&, bool, Render::eRenderPassMode, bool, bool);
    using origSaveBufferForMirror   = bool (*)(Render::GL::CHyprOpenGLImpl*, const CBox&);
    using origRenderMonitor         = void (*)(Render::IHyprRenderer*, PHLMONITOR, bool);
    using origScreenshareRenderMon  = void (*)(Screenshare::CScreenshareFrame*);

    bool hkShouldRenderWindow(Render::IHyprRenderer* self, PHLWINDOW window, PHLMONITOR monitor) {
        const bool original = (*(origShouldRenderWindow)g_shouldRenderWindowHook->m_original)(self, window, monitor);

        if (!original)
            return false;

        // During a screencopy frame CScreenshareFrame::renderMonitor() asks
        // shouldRenderWindow() which windows need a black box.  We have removed
        // the window from the mirror instead, so suppress that black box.  The
        // normal render must return true here so renderWindow() gets called and
        // can record the window for re-adding.
        if (g_inScreenshareRender && isNoScreenShare(window))
            return false;

        return true;
    }

    void hkScreenshareRenderMonitor(Screenshare::CScreenshareFrame* self) {
        g_inScreenshareRender = true;
        (*(origScreenshareRenderMon)g_screenshareRenderHook->m_original)(self);
        g_inScreenshareRender = false;
    }

    void hkRenderWindow(Render::IHyprRenderer* self, PHLWINDOW window, PHLMONITOR monitor, const Time::steady_tp& time, bool decorate,
                        Render::eRenderPassMode mode, bool ignorePosition, bool standalone) {
        if (g_inMainRender && !g_inScreenshareRender && !g_reAdding && isCaptured(monitor) && isNoScreenShare(window)) {
            if (std::ranges::find(g_skippedWindows, window) == g_skippedWindows.end())
                g_skippedWindows.emplace_back(window);

            return;
        }

        (*(origRenderWindow)g_renderWindowHook->m_original)(self, window, monitor, time, decorate, mode, ignorePosition, standalone);
    }

    // Draw one window's surface textures directly into the current output
    // framebuffer.
    //
    // We deliberately do not route this through renderWindow() / the normal
    // pass here.  The extra pass runs inside CHyprOpenGLImpl::end(), after the
    // final screen shader has been enabled.  renderTextureInternal() would try
    // to apply that shader and dereferences image descriptions that internal
    // blur textures may not have.  renderTexturePrimitive() uses the simple
    // passthrough shader, which is exactly what the terminal surface needs.
    //
    // This is a raster fallback for a marked floating terminal: it preserves
    // the terminal contents, but not Hyprland's window decorations, rounding
    // or blur.  Those are cosmetic; the important properties are that the
    // terminal stays readable locally and never enters the mirror.
    void drawWindowSurface(PHLWINDOW window, PHLMONITOR monitor) {
        const auto WLSurface = window->wlSurface();
        if (!WLSurface)
            return;

        const auto RESOURCE = WLSurface->resource();
        if (!RESOURCE)
            return;

        const auto WORKSPACE = window->m_workspace;
        const auto REALPOS   = window->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT) +
            (window->m_pinned || !WORKSPACE ? Vector2D{} : WORKSPACE->m_renderOffset->value()) + window->m_floatingOffset;
        const auto REALSIZE = window->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);

        RESOURCE->breadthfirst(
            [&](SP<CWLSurfaceResource> surface, const Vector2D& offset, void*) {
                if (!surface || !surface->m_current.texture)
                    return;

                if (surface->m_current.size.x < 1 || surface->m_current.size.y < 1)
                    return;

                CSurfacePassElement::SRenderData data;
                data.pMonitor      = monitor;
                data.pos           = REALPOS;
                data.localPos      = offset;
                data.w             = REALSIZE.x;
                data.h             = REALSIZE.y;
                data.surface       = surface;
                data.texture       = surface->m_current.texture;
                data.mainSurface   = surface == RESOURCE;
                data.pWindow       = window;
                data.squishOversized = true;

                CSurfacePassElement element(data);
                const CBox          box = element.getTexBox();
                Render::GL::g_pHyprOpenGL->renderTexturePrimitive(surface->m_current.texture, box);
            },
            nullptr);
    }

    bool hkSaveBufferForMirror(Render::GL::CHyprOpenGLImpl* self, const CBox& box) {
        const bool copied = (*(origSaveBufferForMirror)g_saveMirrorHook->m_original)(self, box);

        if (g_skippedWindows.empty())
            return copied;

        auto windows = std::move(g_skippedWindows);
        g_skippedWindows.clear();

        if (!g_pHyprRenderer || !Render::GL::g_pHyprOpenGL || !g_pHyprRenderer->m_renderData.currentFB)
            return copied;

        const auto monitor = g_pHyprRenderer->m_renderData.pMonitor.lock();
        if (!monitor)
            return copied;

        // The capture session may have stopped between the scene render and
        // this point (short-lived screencopy clients such as grim do exactly
        // that).  We still have to put the windows back into the local output;
        // the mirror copy above is already done and remains clean.

        // saveBufferForMirror() has already copied the clean scene into the
        // mirror framebuffer.  From here on we are only touching the normal
        // output framebuffer, which is blitted to the physical output by end().
        //
        // end() has enabled the monitor transform for its final blit.  The
        // windows were laid out in untransformed monitor space, so turn the
        // transform back off for this extra pass.
        g_reAdding = true;
        g_pHyprRenderer->pushMonitorTransformEnabled(false);

        for (const auto& window : windows) {
            if (!window || !window->m_isMapped)
                continue;

            drawWindowSurface(window, monitor);
        }

        g_pHyprRenderer->popMonitorTransformEnabled();
        g_reAdding = false;

        return copied;
    }

    void hkRenderMonitor(Render::IHyprRenderer* self, PHLMONITOR monitor, bool commit) {
        if (monitor && isCaptured(monitor)) {
            if (!g_capturedMonitors.contains(monitor->m_name)) {
                g_capturedMonitors.insert(monitor->m_name);

                // The first frame after a capture session appears must repair the
                // mirror, even if the last pre-capture frame left a valid mirror
                // containing the terminal.
                monitor->m_forceFullFrames = std::max(monitor->m_forceFullFrames, 1);
            }
        }
        else if (monitor) {
            g_capturedMonitors.erase(monitor->m_name);
        }

        g_inMainRender = true;
        g_skippedWindows.clear();

        (*(origRenderMonitor)g_renderMonitorHook->m_original)(self, monitor, commit);

        g_skippedWindows.clear();
        g_inMainRender = false;
    }

    template <typename Filter>
    void* findHook(const std::string& name, Filter&& filter) {
        const auto matches = HyprlandAPI::findFunctionsByName(g_pluginHandle, name);
        for (const auto& match : matches) {
            if (match.address && filter(match))
                return match.address;
        }
        return nullptr;
    }

} // namespace

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    g_pluginHandle = handle;

    const std::string hash = __hyprland_api_get_hash();
    const std::string clientHash = __hyprland_api_get_client_hash();

    if (hash != clientHash) {
        HyprlandAPI::addNotification(handle,
                                     "[afterflow] Version mismatch: plugin headers do not match the running Hyprland. Rebuild the plugin.",
                                     CHyprColor{1.0, 0.2, 0.2, 1.0}, 7000);
        throw std::runtime_error("[afterflow] Hyprland version mismatch");
    }

    auto* shouldRenderWindow = findHook("shouldRenderWindow", [](const SFunctionMatch& match) {
        return match.demangled.contains("IHyprRenderer::shouldRenderWindow") && match.demangled.contains("CWindow") && match.demangled.contains("CMonitor");
    });

    auto* renderWindow = findHook("renderWindow", [](const SFunctionMatch& match) {
        return match.demangled.contains("IHyprRenderer::renderWindow") && match.demangled.contains("CWindow") && match.demangled.contains("CMonitor");
    });

    auto* saveBufferForMirror = findHook("saveBufferForMirror", [](const SFunctionMatch& match) {
        return match.demangled.contains("CHyprOpenGLImpl::saveBufferForMirror");
    });

    auto* renderMonitor = findHook("renderMonitor", [](const SFunctionMatch& match) {
        return match.demangled.contains("IHyprRenderer::renderMonitor") && match.demangled.contains("CMonitor");
    });

    auto* screenshareRenderMonitor = findHook("renderMonitor", [](const SFunctionMatch& match) {
        return match.demangled.contains("Screenshare::CScreenshareFrame::renderMonitor");
    });

    if (!shouldRenderWindow || !renderWindow || !saveBufferForMirror || !renderMonitor || !screenshareRenderMonitor) {
        HyprlandAPI::addNotification(handle, "[afterflow] Could not find the required Hyprland render functions. The plugin was not loaded.",
                                     CHyprColor{1.0, 0.2, 0.2, 1.0}, 7000);
        throw std::runtime_error("[afterflow] required Hyprland symbols not found");
    }

    g_shouldRenderWindowHook  = HyprlandAPI::createFunctionHook(handle, shouldRenderWindow, reinterpret_cast<void*>(&hkShouldRenderWindow));
    g_renderWindowHook        = HyprlandAPI::createFunctionHook(handle, renderWindow, reinterpret_cast<void*>(&hkRenderWindow));
    g_saveMirrorHook          = HyprlandAPI::createFunctionHook(handle, saveBufferForMirror, reinterpret_cast<void*>(&hkSaveBufferForMirror));
    g_renderMonitorHook       = HyprlandAPI::createFunctionHook(handle, renderMonitor, reinterpret_cast<void*>(&hkRenderMonitor));
    g_screenshareRenderHook   = HyprlandAPI::createFunctionHook(handle, screenshareRenderMonitor, reinterpret_cast<void*>(&hkScreenshareRenderMonitor));

    if (!g_shouldRenderWindowHook || !g_renderWindowHook || !g_saveMirrorHook || !g_renderMonitorHook || !g_screenshareRenderHook) {
        HyprlandAPI::addNotification(handle, "[afterflow] Failed to create one or more render hooks. The plugin was not loaded.",
                                     CHyprColor{1.0, 0.2, 0.2, 1.0}, 7000);
        throw std::runtime_error("[afterflow] failed to create hooks");
    }

    if (!g_shouldRenderWindowHook->hook() || !g_renderWindowHook->hook() || !g_saveMirrorHook->hook() || !g_renderMonitorHook->hook() ||
        !g_screenshareRenderHook->hook()) {
        HyprlandAPI::addNotification(handle, "[afterflow] Failed to install one or more render hooks. The plugin was not loaded.",
                                     CHyprColor{1.0, 0.2, 0.2, 1.0}, 7000);
        throw std::runtime_error("[afterflow] failed to install hooks");
    }

    HyprlandAPI::addNotification(handle, "[afterflow] Capture exclusion active. Marked windows stay visible locally and are absent from monitor captures.",
                                 CHyprColor{0.2, 1.0, 0.4, 1.0}, 5000);

    return {"afterflow-capture-exclusion", "Keep Afterflow's terminal visible locally while excluding it from monitor captures", "Afterflow", "1.0.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    if (g_shouldRenderWindowHook)
        g_shouldRenderWindowHook->unhook();
    if (g_renderWindowHook)
        g_renderWindowHook->unhook();
    if (g_saveMirrorHook)
        g_saveMirrorHook->unhook();
    if (g_renderMonitorHook)
        g_renderMonitorHook->unhook();
    if (g_screenshareRenderHook)
        g_screenshareRenderHook->unhook();

    g_skippedWindows.clear();
    g_capturedMonitors.clear();
}
