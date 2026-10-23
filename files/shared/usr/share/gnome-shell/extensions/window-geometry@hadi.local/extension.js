// Window Geometry — GNOME Shell 50 (ESM).
//
// Restore a window's saved position / size / workspace / maximize / fullscreen
// across reboots with ZERO visible teleport: the window is never painted, for
// even one frame, anywhere other than its final place.
//
// ── Why this can beat the first frame (mutter-50.4, verified by frame capture) ──
//
// smart-auto-move teleports because it hooks `window-created` and later calls
// move_resize_frame() — by which point Mutter has already mapped and animated
// the window at its default placement. Two independent causes: it waits for the
// title to stabilise (it has no identity at window-created), and it never gets
// ahead of the map paint.
//
// Both disappear if we use a different hook. For a native-Wayland xdg-toplevel
// the lifecycle is:
//
//   1. get_toplevel  → meta_wayland_shell_surface_set_window() emits
//      `window-created`. At THIS instant wm_class/app_id/title are still null,
//      which is exactly why smart-auto-move can't decide here.
//   2. the client sets app_id/title, then commits once WITH NO BUFFER. That
//      first commit runs meta_wayland_xdg_toplevel_apply_state(), which — since
//      no configure has been sent yet — emits the `MetaWindow::configure`
//      signal (initial) and then calls meta_window_apply_config(). By now
//      wm_class IS set.
//   3. whatever geometry sits in the MetaWindowConfig when our handler returns
//      is what Mutter puts in the FIRST configure event sent to the client. The
//      client renders at that size and only THEN commits its first buffer.
//
// So placing inside the initial `configure` handler lands the geometry before
// the client has drawn anything. The actor is shown at opacity 0 and GNOME's
// map animation scales it up FROM the final position — captured at 60fps, every
// painted frame of GTK4 / Qt / Electron toplevels sits inside the target rect;
// none anywhere else. No actor hiding or opacity trick is needed — placement
// wins the race outright, so there is no "stuck invisible" failure mode to
// guard against. (If a toolkit ever set app_id *after* its first buffer, we'd
// simply fail to match and leave Mutter's default placement — a documented,
// teleport-free degradation, never a late move.)
//
// `meta_window_emit_configure` has exactly one Wayland caller (that initial
// path), so the signal fires once per toplevel. THIS ORDERING IS THE LOAD-
// BEARING ASSUMPTION and the thing most likely to shift on a future GNOME: if a
// release starts sending the initial configure before wm_class is set, or stops
// routing initial placement through MetaWindowConfig, restore will silently
// regress to a teleport. Re-verify with a frame capture when bumping GNOME.
// Mutter 51 already broke the position half (windows mapped at 0,0); a floating
// restore now re-asserts its rect on `shown` (see _restoreInto / _settle).
//
// State lives in a plain JSON file (see stateFile()), written atomically. Not
// GSettings/dconf on purpose: no gschema to compile into the nix derivation,
// and — unlike smart-auto-move's `saved-windows` key — no dconf subtree for
// home-manager to stomp on activation (see the note in gnome-dconf.nix).

import Meta from 'gi://Meta';
import GLib from 'gi://GLib';
import Gio from 'gi://Gio';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

const STATE_VERSION = 2;

// Coalesce a drag/resize burst (many position/size-changed in a row) into a
// single disk write.
const SAVE_DEBOUNCE_MS = 500;

// After we restore a MAXIMIZED window, Mutter's own saved_rect (the geometry an
// unmaximize returns to) is captured while the window is still 0x0, so a later
// user-unmaximize would snap to x=0. We keep our own floating rect and, for a
// bounded grace period after restore, snap the first unmaximize to it. Bounded
// so it can never fire late in the session and surprise the user.
const UNMAX_SNAP_GRACE_MS = 5000;

// Several windows of one class are restored into slots in appearance order, and
// an app's session restore need not open them in the order we saved them (Brave
// swapped two split-view windows). The real page title isn't known at the
// initial configure, so the order can't be fixed pre-frame. Instead each entry
// saves its title, and for a bounded period after restore, once a window's real
// title arrives, a window that matches ANOTHER restored window's saved title
// swaps places with it — one visible correction, only when the order was wrong.
const TITLE_MATCH_GRACE_MS = 15000;

// Leading unread counters like "(3) " change between sessions; ignore them.
function normTitle(t) {
    return (t || '').replace(/^\(\d+\)\s*/, '').trim();
}

// Browser Picture-in-Picture windows are saved/restored under their OWN key,
// never under the browser's wm_class. brave-origin gives the PiP toplevel the
// SAME wm_class as its normal windows, so keying by wm_class made PiP take the
// next free browser slot and inherit a full browser window's size. (Older
// Chromium left PiP's wm_class empty; the title test covers both.) The title is
// checked BEFORE the wm_class, so PiP can never land in a browser slot. Unlike a
// normal browser window's page-derived title, the PiP title is already set at
// the initial configure (verified by capture). With no saved PiP geometry yet,
// PiP spawns in the bottom-right corner of the work area. The title strings
// mirror the ones the pip-on-top extension keys off, so the two agree on what
// PiP is.
const PIP_KEY = '__pip__';
const PIP_TITLES = ['picture in picture', 'picture-in-picture'];
const PIP_MARGIN = 32;            // logical px from the work area's bottom-right

// PiP aspect lock. xdg-shell has no aspect-ratio hint (Chromium sets one only
// on X11), and Mutter's constrain_aspect_ratio reads X11 size hints only, so a
// Wayland PiP resizes freely and letterboxes. Correcting on every size-changed
// DURING the drag would fight the grab (Mutter recomputes from the pointer on
// each motion → jitter), so instead the size is snapped to 16:9 once, when the
// resize grab ends: one correction on release, keeping the corner opposite the
// dragged edge fixed. The compositor never sees the video's real ratio, so this
// is a fixed 16:9, not per-video.
const PIP_ASPECT = 16 / 9;
// MetaGrabOp direction bits (verified against Meta-18's GrabOp values).
const DIR_W = 0x1000, DIR_E = 0x2000, DIR_S = 0x4000, DIR_N = 0x8000;

function isPip(win) {
    const title = (win.get_title() || '').toLowerCase();
    return PIP_TITLES.includes(title) || title.endsWith(' - pip');
}

// A fixed-size toplevel (min == max size: a splash screen, some game launchers
// and utilities) shares its app's wm_class. Keyed by wm_class alone it took the
// app's slot 0, so the main window inherited its geometry whenever it mapped
// alone (Discord reopened from the tray — a hidden Wayland toplevel is a new
// MetaWindow — landed in the splash's 800x500). Such windows are filed under
// their own key instead, so both kinds restore and can never collide. The
// hints often land only after the initial configure, so a window's key may
// change until it is first shown (see _refreshIdentity).
const FIXED_SUFFIX = '#fixed';

// `resizeable` alone does not mean min == max size: Mutter also clears it
// while a window is fullscreen (meta_window_recalc_features, mutter-50.5
// window.c:6048). Read as "fixed", restoring a saved-fullscreen window flipped
// its key to FIXED_SUFFIX, the re-key undid the fullscreen, the window became
// resizeable again and was re-keyed back into its fullscreen slot — forever,
// inside the Shell's main loop (mpv froze the whole desktop). A fullscreen
// window's fixed-ness is unknowable, so it counts as resizable.
function isFixedSize(win) {
    return !win.resizeable && !win.fullscreen;
}

// Backstop for any other feature flip-flop: a window not yet shown is re-filed
// at most this many times, then keeps its current key. A wrong key is a
// cosmetic slot mix-up; an unbounded re-key loop hangs the compositor.
const MAX_REKEYS = 3;

function stateFile() {
    // ~/.local/state/window-geometry/state.json
    return GLib.build_filenamev([GLib.get_user_state_dir(), 'window-geometry', 'state.json']);
}

// Copy the fields out of the boxed MtkRectangle immediately. NB: reading the
// MetaWindowConfig `rect` *property* getter crashes GJS (boxed struct returned
// by value), but get_frame_rect() — a plain method — is fine.
function frameRect(win) {
    const r = win.get_frame_rect();
    return {x: r.x, y: r.y, w: r.width, h: r.height};
}

export default class WindowGeometryExtension extends Extension {
    enable() {
        this._state = this._load();          // {version, apps: {wm_class: [entry, …]}}
        this._records = new Map();           // MetaWindow -> tracking record
        this._pendingConfigure = new Map();  // MetaWindow -> handler id (one-shot, pre-restore)
        this._slotsInUse = new Map();        // wm_class -> Set<int>  (live slot ids)
        this._pipIds = new Map();            // MetaWindow -> handler id (one-shot PiP placement)
        this._saveTimer = 0;
        this._pipResize = new Map();         // MetaWindow -> {op, start} (PiP resize in progress)
        this._aspectIdles = new Set();       // pending snap idle sources

        this._createdId = global.display.connect('window-created',
            (_d, win) => this._onWindowCreated(win));
        this._grabBeginId = global.display.connect('grab-op-begin',
            (_d, win, op) => this._onGrabBegin(win, op));
        this._grabEndId = global.display.connect('grab-op-end',
            (_d, win) => this._onGrabEnd(win));

        // Re-enabled mid-session (not a fresh login): windows already exist and
        // are already placed — we can't zero-teleport them, so only adopt the
        // manageable ones for SAVING. On a real login this loop is empty and
        // every window comes through window-created, where restore happens.
        for (const actor of global.get_window_actors()) {
            if (this._isManageable(actor.metaWindow))
                this._adopt(actor.metaWindow, true);
        }
    }

    disable() {
        // A leaked signal handler survives a Shell reload and is a real bug on
        // Wayland, so tear EVERYTHING down explicitly.
        if (this._createdId)
            global.display.disconnect(this._createdId);
        this._createdId = 0;
        for (const id of [this._grabBeginId, this._grabEndId])
            if (id)
                global.display.disconnect(id);
        this._grabBeginId = this._grabEndId = 0;
        for (const id of this._aspectIdles)
            GLib.source_remove(id);
        this._aspectIdles.clear();
        this._pipResize.clear();

        for (const [win, id] of this._pendingConfigure)
            try { win.disconnect(id); } catch { /* window already gone */ }
        this._pendingConfigure.clear();

        for (const [win, id] of this._pipIds)
            try { win.disconnect(id); } catch { /* window already gone */ }
        this._pipIds.clear();

        for (const rec of this._records.values())
            this._teardownRecord(rec);
        this._records.clear();

        // Flush any pending debounced write synchronously so nothing is lost.
        if (this._saveTimer) {
            GLib.source_remove(this._saveTimer);
            this._saveTimer = 0;
            this._writeState();
        }

        this._slotsInUse.clear();
        this._state = null;
    }

    // ───────────────────────────── restore path ─────────────────────────────

    _isManageable(win) {
        // Native Wayland only (per requirement; the XWayland configure timing
        // differs and we don't support it). Skip dialogs, transients and
        // override-redirect — only real top-levels we can give a stable
        // identity to (a wm_class, possibly FIXED_SUFFIX'd, or PIP_KEY).
        //
        // NB: PiP is deliberately NOT excluded by the skip-taskbar test — the
        // browser leaves PiP taskbar-visible (verified: skip_taskbar=false), so
        // the test still filters genuine utility pop-ups without catching PiP.
        return win.get_client_type() === Meta.WindowClientType.WAYLAND &&
            win.get_window_type() === Meta.WindowType.NORMAL &&
            !win.is_override_redirect() &&
            win.get_transient_for() === null &&
            !win.is_skip_taskbar() &&
            this._identity(win) !== null;
    }

    // The per-window key we save/restore under: PIP_KEY for a browser PiP
    // window, else the wm_class, suffixed for a fixed-size window. null means
    // "we have no reliable handle, leave Mutter's default placement". `fixed`
    // is passed explicitly by a record whose identity is frozen.
    _identity(win, fixed = isFixedSize(win)) {
        if (isPip(win))
            return PIP_KEY;
        const cls = win.get_wm_class();
        if (typeof cls !== 'string' || cls.length === 0)
            return null;
        return fixed ? cls + FIXED_SUFFIX : cls;
    }

    // First-ever PiP (nothing saved): the client picks its size with its first
    // buffer, so the corner position can't go into the initial configure. `shown` fires once that
    // buffer is committed (size known) and before the next paint, so the move
    // lands before the window is ever drawn at Mutter's default placement.
    _placePip(win) {
        if (this._pipIds.has(win))
            return;
        const id = win.connect('shown', () => {
            win.disconnect(id);
            this._pipIds.delete(win);
            this._movePipToCorner(win);
        });
        this._pipIds.set(win, id);
    }

    _movePipToCorner(win) {
        const wa = win.get_work_area_current_monitor();
        const r = win.get_frame_rect();
        win.move_frame(false,
            wa.x + wa.width - r.width - PIP_MARGIN,
            wa.y + wa.height - r.height - PIP_MARGIN);
    }

    // The key a window is filed under can change after adoption: the fixed-
    // size hints (and, as a safety net, a PiP title) may land only after the
    // initial configure. Until the window is first shown its fixed-ness is
    // re-read; after that it is frozen, so an app that toggles resizability
    // mid-session (Electron during a call, SDL between menu and game) keeps
    // its slot and its saved geometry. Returns the live record for the window
    // — a new one if it was re-filed.
    _refreshIdentity(rec) {
        const fixed = rec.settled ? rec.fixed : isFixedSize(rec.win);
        if (this._identity(rec.win, fixed) === rec.cls)
            return rec;
        if (rec.rekeys >= MAX_REKEYS) {
            if (!rec.rekeyCapped) {
                rec.rekeyCapped = true;
                log(`window-geometry: ${rec.cls} keeps changing key, leaving it as is`);
            }
            return rec;
        }
        return this._rekey(rec);
    }

    // Re-file a window under its current key WITHOUT saving it under the old
    // one (the browser slot keeps its real geometry when a window turns out to
    // be PiP; the app's slot never sees its splash). A window not yet shown is
    // then placed for its new key: its saved entry if there is one, otherwise
    // the restore done for the old key is undone — the main window's maximize
    // or workspace must not stick to the splash. A window already on screen
    // stays where it is.
    _rekey(rec) {
        const win = rec.win;
        const {settled, placed} = rec;
        this._forget(rec);
        const next = this._adopt(win, settled);
        next.rekeys = rec.rekeys + 1;
        const entry = this._entryFor(next.cls, next.slot);
        if (entry) {
            this._applyEntry(next, entry);
            this._armTitleCheck(next, entry);
        } else if (next.cls === PIP_KEY) {
            if (settled)
                this._movePipToCorner(win);
            else
                this._placePip(win);
        } else if (!settled && placed) {
            this._undoRestore(next, placed);
        }
        return next;
    }

    // Reverse what a restore did to a window that turned out not to belong to
    // that entry. The position is fixed up on `shown`, once the window's real
    // (fixed) size is known: it is centred on its monitor like a fresh splash.
    _undoRestore(rec, placed) {
        const win = rec.win;
        if (placed.fullscreen)
            win.unmake_fullscreen();
        if (placed.maximized)
            win.unmaximize();
        if (placed.workspace)
            win.change_workspace(global.workspace_manager.get_active_workspace());
        rec.floatRect = null;
        rec.center = true;
    }

    // First map: the identity is final from here on and the window becomes
    // saveable (before this its frame is only what our restore configured, so
    // nothing of its own would be written). NB: `shown` only fires once the
    // window is actually showing, so a window restored onto another workspace
    // settles when that workspace is first viewed.
    _settle(rec) {
        const cur = this._refreshIdentity(rec);
        if (cur.center) {
            cur.center = false;
            this._center(cur.win);
        }
        const want = cur.restoreRect;
        cur.restoreRect = null;
        if (want && !cur.win.maximized_horizontally && !cur.win.maximized_vertically &&
            !cur.win.fullscreen) {
            const r = frameRect(cur.win);
            if (r.x !== want.x || r.y !== want.y || r.w !== want.w || r.h !== want.h)
                cur.win.move_resize_frame(false, want.x, want.y, want.w, want.h);
        }
        cur.settled = true;
        this._scheduleSave();
    }

    _center(win) {
        const wa = win.get_work_area_current_monitor();
        const r = win.get_frame_rect();
        win.move_frame(false,
            wa.x + Math.round((wa.width - r.width) / 2),
            wa.y + Math.round((wa.height - r.height) / 2));
    }

    _onWindowCreated(win) {
        // wm_class is null here; wait for the initial `configure`, which fires
        // on the client's first (buffer-less) commit, by which point wm_class
        // is set and the geometry we write still precedes the first buffer.
        let cfgId = win.connect('configure', (_w, config) => {
            // One-shot: only the initial configure carries the placement we can
            // still influence. Disconnect first so a re-entrant emission or a
            // throw below can't leave the handler attached.
            win.disconnect(cfgId);
            this._pendingConfigure.delete(win);
            cfgId = 0;

            if (!config.get_is_initial())
                return;
            if (!this._isManageable(win))
                return;

            const rec = this._adopt(win);
            try {
                this._restoreInto(win, config, rec);
            } catch (e) {
                logError(e, 'window-geometry: restore failed');
            }
        });
        this._pendingConfigure.set(win, cfgId);
    }

    _restoreInto(win, config, rec) {
        const cls = rec.cls;
        const entry = this._entryFor(cls, rec.slot);
        if (!entry) {
            // Unknown window / no saved slot → leave Mutter's default, except
            // a first-ever PiP, which goes to the bottom-right corner.
            if (cls === PIP_KEY)
                this._placePip(win);
            return;
        }
        const {x, y, w, h} = entry;

        // Remember the floating rect for save-while-maximized and the unmax
        // snap, and what we change so a re-key can undo it (see _rekey).
        rec.floatRect = {x, y, w, h};
        rec.placed = {workspace: false, maximized: false, fullscreen: false};

        // Workspace first. Clamp into the existing range rather than appending
        // empties (this host runs dynamic workspaces + workspaces-only-on-
        // primary); a saved index past the current count just restores to the
        // last one.
        if (Number.isInteger(entry.workspace) && entry.workspace >= 0) {
            const n = global.workspace_manager.get_n_workspaces();
            const target = Math.max(0, Math.min(entry.workspace, n - 1));
            rec.placed.workspace = target !== global.workspace_manager.get_active_workspace_index();
            win.change_workspace_by_index(target, false);
        }

        // Always seed the floating rect into the config first: for a maximized
        // or fullscreen restore this is the geometry the window should return to.
        config.set_position(x, y);
        config.set_size(w, h);

        if (entry.fullscreen) {
            config.set_is_fullscreen(true);
            rec.placed.fullscreen = true;
        } else if (!entry.maximized) {
            // Mutter 51 no longer carries this position through to the first
            // buffer: our config marks the window placed (so Mutter skips its
            // own placement) but the acked configuration has no position, and
            // the window maps at the monitor's 0,0 — under the top bar. The
            // size still lands pre-frame; `shown` puts the position right
            // before the first paint (see _settle).
            rec.restoreRect = {x, y, w, h};
        } else {
            // Maps straight to maximized with no teleport (the map animation
            // scales up the maximized frame in place). Arm the bounded unmax
            // snap so the FIRST user-unmaximize returns to the floating rect
            // instead of Mutter's bogus 0,0 saved_rect.
            // NB: meta_window_maximize() takes no flags in Mutter 50 (BOTH is
            // implied); the directional form is set_maximize_flags().
            win.maximize();
            rec.snapUntil = GLib.get_monotonic_time() + UNMAX_SNAP_GRACE_MS * 1000;
            rec.placed.maximized = true;
        }

        this._armTitleCheck(rec, entry);
    }

    // The saved entry for a slot with its rect resolved to absolute
    // coordinates, or null. Monitor identity is by connector name, never index
    // — indices renumber across hotplug and DP wake; an absent connector falls
    // back to primary. The rect is stored RELATIVE to its monitor's top-left,
    // so a monitor that moved in the layout (or a different-but-present
    // connector) still lands the window in the right place on that monitor.
    _entryFor(cls, slot) {
        const entry = this._state?.apps?.[cls]?.[slot];
        if (!entry)
            return null;
        const mon = Main.layoutManager.monitors[this._indexForConnector(entry.connector)];
        if (!mon)
            return null; // no monitors at all — nothing sensible to do.
        return {...entry, x: mon.x + entry.rx, y: mon.y + entry.ry};
    }

    // Arm the title check (see TITLE_MATCH_GRACE_MS). Pointless for a single-
    // window class or an entry saved before titles were recorded.
    _armTitleCheck(rec, entry) {
        if (rec.cls === PIP_KEY || !entry.title || this._state.apps[rec.cls].length <= 1)
            return;
        this._setExpect(rec, {
            title: entry.title,
            entry,
            until: GLib.get_monotonic_time() + TITLE_MATCH_GRACE_MS * 1000,
        });
    }

    // `expect` is truthy exactly while the grace period runs: a timer gives
    // the title one last look and then clears it, so no reader has to compare
    // deadlines and a window whose title never changes can't carry a stale
    // expectation (or the saved title, see _writeEntry) for the whole session.
    _setExpect(rec, exp) {
        this._clearExpect(rec);
        if (!exp)
            return;
        rec.expect = exp;
        const left = Math.max(0, Math.ceil((exp.until - GLib.get_monotonic_time()) / 1000));
        rec.expectTimer = GLib.timeout_add(GLib.PRIORITY_DEFAULT, left, () => {
            rec.expectTimer = 0;
            this._checkTitleMatch(rec);
            this._clearExpect(rec);
            return GLib.SOURCE_REMOVE;
        });
    }

    _clearExpect(rec) {
        if (rec.expectTimer)
            GLib.source_remove(rec.expectTimer);
        rec.expectTimer = 0;
        rec.expect = null;
    }

    // A restored window's title changed: if it now matches another pending
    // window's saved title (and not its own), the two were restored into each
    // other's slots — swap them. Settled windows drop out of the check.
    _checkTitleMatch(rec) {
        const exp = rec.expect;
        if (!exp)
            return;
        const title = normTitle(rec.win.get_title());
        if (!title)
            return;
        if (title === normTitle(exp.title)) {
            this._clearExpect(rec); // in the right slot
            return;
        }
        for (const other of this._records.values()) {
            if (other === rec || other.cls !== rec.cls || !other.expect)
                continue;
            if (normTitle(other.expect.title) !== title)
                continue;
            [rec.slot, other.slot] = [other.slot, rec.slot];
            const theirs = other.expect;
            this._clearExpect(rec); // its title now matches its slot
            this._setExpect(other, exp);
            this._applyEntry(rec, theirs.entry);
            this._applyEntry(other, exp.entry);
            this._checkTitleMatch(other); // its title may already be final
            this._scheduleSave();
            return;
        }
    }

    // Move an already-mapped window to a restored entry (the post-map
    // counterpart of the config writes in _restoreInto).
    _applyEntry(rec, e) {
        const win = rec.win;
        if (win.fullscreen && !e.fullscreen)
            win.unmake_fullscreen();
        if ((win.maximized_horizontally || win.maximized_vertically) && !e.maximized)
            win.unmaximize();
        rec.floatRect = {x: e.x, y: e.y, w: e.w, h: e.h};
        rec.snapUntil = 0;
        rec.placed = {workspace: false, maximized: !!e.maximized, fullscreen: !!e.fullscreen};
        win.move_resize_frame(false, e.x, e.y, e.w, e.h);
        if (Number.isInteger(e.workspace) && e.workspace >= 0) {
            const n = global.workspace_manager.get_n_workspaces();
            const target = Math.max(0, Math.min(e.workspace, n - 1));
            rec.placed.workspace = target !== global.workspace_manager.get_active_workspace_index();
            win.change_workspace_by_index(target, false);
        }
        if (e.fullscreen)
            win.make_fullscreen();
        else if (e.maximized)
            win.maximize();
    }

    // ─────────────────────────── PiP aspect lock ────────────────────────────

    _onGrabBegin(win, op) {
        const resizing = op & (DIR_W | DIR_E | DIR_S | DIR_N) ||
            op === Meta.GrabOp.KEYBOARD_RESIZING_UNKNOWN;
        if (win && resizing && isPip(win))
            this._pipResize.set(win, {op, start: frameRect(win)});
    }

    _onGrabEnd(win) {
        const grab = win && this._pipResize.get(win);
        if (!grab)
            return;
        this._pipResize.delete(win);
        // Defer past the grab teardown so our move_resize isn't applied with
        // the grab's resize gravity still in effect.
        const id = GLib.idle_add(GLib.PRIORITY_DEFAULT, () => {
            this._aspectIdles.delete(id);
            try {
                this._snapPipAspect(win, grab);
            } catch (e) {
                logError(e, 'window-geometry: PiP aspect snap failed');
            }
            return GLib.SOURCE_REMOVE;
        });
        this._aspectIdles.add(id);
    }

    _snapPipAspect(win, {op, start}) {
        if (!win.get_compositor_private() || win.fullscreen ||
            win.maximized_horizontally || win.maximized_vertically)
            return;
        const r = frameRect(win);
        const horiz = op & (DIR_W | DIR_E), vert = op & (DIR_N | DIR_S);

        // The dimension the user actually drove leads; the other follows. An
        // edge drag drives one axis; a corner (or a keyboard resize, whose
        // direction isn't known at grab begin) drives whichever changed more.
        let widthLeads;
        if (horiz && !vert)
            widthLeads = true;
        else if (vert && !horiz)
            widthLeads = false;
        else
            widthLeads = Math.abs(r.w - start.w) / start.w >= Math.abs(r.h - start.h) / start.h;

        let w = widthLeads ? r.w : Math.round(r.h * PIP_ASPECT);
        let h = widthLeads ? Math.round(r.w / PIP_ASPECT) : r.h;

        // Never outgrow the work area: scale down, keeping the ratio.
        const wa = win.get_work_area_current_monitor();
        const fit = Math.min(1, wa.width / w, wa.height / h);
        w = Math.round(w * fit);
        h = Math.round(h * fit);
        if (Math.abs(w - r.w) <= 1 && Math.abs(h - r.h) <= 1)
            return;

        // Keep the edge opposite the dragged one fixed, then shift back inside
        // the work area (a bottom-right PiP grown by its left edge would
        // otherwise push its bottom off-screen).
        let x = op & DIR_W ? r.x + r.w - w : r.x;
        let y = op & DIR_N ? r.y + r.h - h : r.y;
        x = Math.max(wa.x, Math.min(x, wa.x + wa.width - w));
        y = Math.max(wa.y, Math.min(y, wa.y + wa.height - h));
        win.move_resize_frame(false, x, y, w, h);
    }

    // ───────────────────────────── save path ────────────────────────────────

    // `settled`: the window is already on screen (adopted mid-session), so its
    // identity is final and it is saveable right away; a window adopted at its
    // initial configure settles on `shown` (see _settle).
    _adopt(win, settled = false) {
        let rec = this._records.get(win);
        if (rec)
            return rec;

        const fixed = isFixedSize(win);
        const cls = this._identity(win, fixed) ?? '';

        // Assign the lowest free slot for this class, so slot ids are stable and
        // reused as windows come and go (indices never churn under us). Single-
        // window apps — the overwhelming common case — are always slot 0, i.e.
        // an exact match. Multiple windows of one class are matched in
        // appearance order (see _compactSlots for the one correction we make);
        // that's a heuristic, and the documented limit of wm_class-only
        // identity.
        let used = this._slotsInUse.get(cls);
        if (!used) {
            used = new Set();
            this._slotsInUse.set(cls, used);
        }
        let slot = 0;
        while (used.has(slot))
            slot++;
        used.add(slot);

        rec = {
            win, cls, fixed, slot, settled,
            ids: [],
            floatRect: null,
            snapUntil: 0,
            expect: null,     // pending title check, see _setExpect
            expectTimer: 0,
            placed: null,     // what the restore changed, see _restoreInto
            restoreRect: null, // floating rect to enforce on `shown`, see _settle
            center: false,    // centre on `shown`, see _undoRestore
            rekeys: 0,        // times re-filed before shown, see MAX_REKEYS
            rekeyCapped: false,
        };

        const track = (obj, sig, cb) => rec.ids.push([obj, obj.connect(sig, cb)]);
        track(win, 'position-changed', () => this._onGeometryChanged(rec));
        track(win, 'size-changed', () => this._onGeometryChanged(rec));
        track(win, 'workspace-changed', () => this._scheduleSave());
        track(win, 'notify::maximized-horizontally', () => this._onStateChanged(rec));
        track(win, 'notify::maximized-vertically', () => this._onStateChanged(rec));
        track(win, 'notify::fullscreen', () => this._onStateChanged(rec));
        track(win, 'unmanaged', () => this._onUnmanaged(rec));
        // Until first shown the key may still change (fixed-size hints landing
        // after the initial configure, see FIXED_SUFFIX); `shown` is the last
        // word and makes the window saveable.
        track(win, 'notify::resizeable', () => {
            if (!rec.settled)
                this._refreshIdentity(rec);
        });
        track(win, 'shown', () => {
            if (!rec.settled)
                this._settle(rec);
        });
        // Safety net in case a browser ever sets the PiP title only after the
        // initial configure: the moment the title turns PiP, stop tracking the
        // window under its wm_class without saving it and re-file it as PiP.
        if (cls !== PIP_KEY) {
            track(win, 'notify::title', () => {
                if (this._refreshIdentity(rec) === rec)
                    this._checkTitleMatch(rec);
            });
        }

        this._records.set(win, rec);

        // Seed the floating rect from the current frame if the window is already
        // floating (covers mid-session adoption).
        if (!win.maximized_horizontally && !win.maximized_vertically && !win.fullscreen)
            rec.floatRect = frameRect(win);

        return rec;
    }

    _onGeometryChanged(rec) {
        // Only a floating window's frame IS its floating rect; while maximized
        // or fullscreen, get_frame_rect() is the maximized frame, so keep the
        // last floating rect we saw (this is the "unmaximized rect stored
        // alongside the flags" the restore needs).
        const win = rec.win;
        if (!win.maximized_horizontally && !win.maximized_vertically && !win.fullscreen)
            rec.floatRect = frameRect(win);
        this._scheduleSave();
    }

    _onStateChanged(rec) {
        const win = rec.win;
        const nowFloating = !win.maximized_horizontally &&
            !win.maximized_vertically && !win.fullscreen;

        // Bounded unmax snap: right after a maximized restore, the first return
        // to floating goes to OUR rect, once, then disarms.
        if (rec.snapUntil && nowFloating && rec.floatRect &&
            GLib.get_monotonic_time() < rec.snapUntil) {
            rec.snapUntil = 0;
            const r = rec.floatRect;
            win.move_resize_frame(false, r.x, r.y, r.w, r.h);
        } else if (nowFloating) {
            rec.snapUntil = 0;
        }
        this._scheduleSave();
    }

    _onUnmanaged(rec) {
        // Capture the window's final geometry into the persisted state BEFORE we
        // drop it, so a closed app is still restored next launch. We do NOT
        // remove the entry — closing a window shouldn't forget where it was.
        // A window that was never shown has nothing of its own to save.
        if (rec.settled)
            this._writeEntry(rec);
        this._forget(rec);
        this._scheduleSave();
    }

    // Stop tracking a window WITHOUT saving it, freeing its slot.
    _forget(rec) {
        this._slotsInUse.get(rec.cls)?.delete(rec.slot);
        this._teardownRecord(rec);
        this._records.delete(rec.win);
        this._compactSlots(rec.cls);
    }

    // Slots go by appearance order, and a splash holds its app's slot 0 until
    // its hints arrive and it is re-filed. Had the main window's configure come
    // first it would sit in slot 1 for good — and in slot 0 when reopened from
    // the tray, so the two would drift apart. So when a slot frees up, windows
    // of that class not yet shown (still movable without a visible teleport)
    // shift down into it and take over that slot's entry.
    _compactSlots(cls) {
        const used = this._slotsInUse.get(cls);
        if (!used)
            return;
        const pending = [...this._records.values()]
            .filter(r => r.cls === cls && !r.settled)
            .sort((a, b) => a.slot - b.slot);
        for (const r of pending) {
            let slot = 0;
            while (slot < r.slot && used.has(slot))
                slot++;
            if (slot === r.slot)
                continue;
            used.delete(r.slot);
            used.add(slot);
            r.slot = slot;
            this._clearExpect(r);
            const entry = this._entryFor(cls, slot);
            if (entry) {
                this._applyEntry(r, entry);
                this._armTitleCheck(r, entry);
            }
        }
    }

    _teardownRecord(rec) {
        this._clearExpect(rec);
        for (const [obj, id] of rec.ids)
            try { obj.disconnect(id); } catch { /* already gone */ }
        rec.ids = [];
    }

    // Fold one window's current geometry into this._state (in memory).
    _writeEntry(rec) {
        const win = rec.win;
        if (!this._state)
            return;

        const maximized = win.maximized_horizontally && win.maximized_vertically;
        const fullscreen = win.fullscreen;

        // For maximized/fullscreen store the floating rect (where it should
        // restore to); otherwise the live frame.
        const rect = (maximized || fullscreen)
            ? (rec.floatRect ?? frameRect(win))
            : frameRect(win);

        const monIdx = win.get_monitor();
        const mon = Main.layoutManager.monitors[monIdx];
        if (!mon)
            return; // window has no monitor (being unmapped); skip this write.
        const connector = this._connectorForIndex(monIdx);

        const ws = win.get_workspace();
        const entry = {
            connector,
            rx: rect.x - mon.x,
            ry: rect.y - mon.y,
            w: rect.w,
            h: rect.h,
            maximized,
            fullscreen,
            workspace: ws ? ws.index() : -1,
            // While the title check is pending the live title may still be a
            // placeholder; keep the saved one so a quick re-login can still
            // match. `expect` is cleared when the grace period ends, so this
            // can't stamp the saved title onto a window for the whole session.
            title: rec.expect ? rec.expect.title : win.get_title(),
        };

        if (!this._state.apps[rec.cls])
            this._state.apps[rec.cls] = [];
        this._state.apps[rec.cls][rec.slot] = entry;
    }

    _scheduleSave() {
        if (this._saveTimer)
            return;
        this._saveTimer = GLib.timeout_add(GLib.PRIORITY_DEFAULT, SAVE_DEBOUNCE_MS, () => {
            this._saveTimer = 0;
            // Snapshot every live window that has been shown, then persist.
            for (const rec of this._records.values()) {
                if (rec.settled)
                    this._writeEntry(rec);
            }
            this._writeState();
            return GLib.SOURCE_REMOVE;
        });
    }

    // ─────────────────────────── monitor helpers ────────────────────────────

    // Connector name for a logical-monitor index (the same index get_monitor()
    // and Main.layoutManager.monitors use). A logical monitor can drive several
    // physical outputs (mirroring); the first connector is a stable enough key.
    _connectorForIndex(index) {
        try {
            const mm = global.backend.get_monitor_manager();
            for (const lm of mm.get_logical_monitors()) {
                if (lm.get_number() !== index)
                    continue;
                const mons = lm.get_monitors();
                return mons.length ? mons[0].get_connector() : null;
            }
        } catch (e) {
            logError(e, 'window-geometry: connector lookup failed');
        }
        return null;
    }

    // Logical-monitor index for a saved connector, or the primary index when
    // that connector is absent (hotplug / different setup).
    _indexForConnector(connector) {
        if (connector) {
            try {
                const mm = global.backend.get_monitor_manager();
                const idx = mm.get_monitor_for_connector(connector);
                if (idx >= 0 && idx < Main.layoutManager.monitors.length)
                    return idx;
            } catch (e) {
                logError(e, 'window-geometry: connector resolve failed');
            }
        }
        return Main.layoutManager.primaryIndex;
    }

    // ────────────────────────────── persistence ─────────────────────────────

    _load() {
        const empty = {version: STATE_VERSION, apps: {}};
        try {
            const [ok, bytes] = GLib.file_get_contents(stateFile());
            if (!ok)
                return empty;
            const parsed = JSON.parse(new TextDecoder().decode(bytes));
            if (!parsed || typeof parsed !== 'object' || typeof parsed.apps !== 'object')
                return empty;
            if (!(parsed.version >= 2))
                this._migrateV1(parsed);
            parsed.version = STATE_VERSION;
            return parsed;
        } catch {
            // Missing or corrupt file: start clean rather than fail to enable.
            return empty;
        }
    }

    // v1 filed fixed-size windows under their app's wm_class and could stamp
    // one slot's saved title onto another (the title hold had no expiry), so a
    // class whose entries repeat a title holds a stray slot — which the title
    // check could never tell apart anyway. Keep the first of each title, drop
    // holes left by nulls, and compact; slot ids are appearance order, so
    // renumbering is harmless.
    _migrateV1(state) {
        for (const [cls, entries] of Object.entries(state.apps)) {
            if (!Array.isArray(entries)) {
                delete state.apps[cls];
                continue;
            }
            const seen = new Set();
            state.apps[cls] = entries.filter(e => {
                if (!e || typeof e !== 'object')
                    return false;
                const t = normTitle(e.title);
                if (!t)
                    return true;
                if (seen.has(t))
                    return false;
                seen.add(t);
                return true;
            });
        }
    }

    _writeState() {
        if (!this._state)
            return;
        try {
            const path = stateFile();
            GLib.mkdir_with_parents(GLib.path_get_dirname(path), 0o700);
            // file_set_contents writes to a temp file and renames — atomic on
            // the same filesystem, so a crash mid-write can't corrupt the state.
            GLib.file_set_contents(path, JSON.stringify(this._state, null, '\t'));
        } catch (e) {
            logError(e, 'window-geometry: state write failed');
        }
    }
}
