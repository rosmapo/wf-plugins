/*
 * xkill — a Wayfire plugin that works like the classic X11 `xkill`
 * utility: press a keybinding, the cursor turns into a crosshair, and
 * the next left click on a window force-kills the client that owns it.
 *
 * Targets: Wayfire 0.9.0 (as shipped in Debian 13 "trixie").
 *
 * Implementation notes:
 *  - Activation is a plain keybinding (config option `xkill/activate`).
 *  - While active, the plugin grabs pointer + keyboard input via
 *    wf::input_grab_t (the same mechanism plugins like wrot/cube use
 *    for interactive drag operations) and sets the cursor image to
 *    "crosshair".
 *  - Left click on a toplevel window kills it; right click or Escape
 *    cancels without killing anything.
 *  - For Xwayland windows, the pid must be read from the
 *    wlr_xwayland_surface itself (wlr_xwayland_surface::pid), NOT from
 *    the wl_client that owns the wl_surface resource — that wl_client
 *    is the Xwayland server process, shared by every X11 window, and
 *    killing it would take down all X11 apps at once.
 */

#include <wayfire/per-output-plugin.hpp>
#include <wayfire/plugin.hpp>
#include <wayfire/output.hpp>
#include <wayfire/core.hpp>
#include <wayfire/view.hpp>
#include <wayfire/toplevel-view.hpp>
#include <wayfire/option-wrapper.hpp>
#include <wayfire/util/log.hpp>
#include <wayfire/plugins/common/input-grab.hpp>
#include <wayfire/scene-input.hpp>
#include <wayfire/seat.hpp>
#include <wayfire/nonstd/wlroots-full.hpp>

#include <csignal>
#include <wayland-server-core.h>
#include <linux/input-event-codes.h>

class wayfire_xkill_t : public wf::per_output_plugin_instance_t,
    public wf::pointer_interaction_t,
    public wf::keyboard_interaction_t
{
    wf::option_wrapper_t<wf::keybinding_t> activate_key{"xkill/activate"};

    std::unique_ptr<wf::input_grab_t> input_grab;
    bool active = false;

    wf::plugin_activation_data_t grab_interface = {
        .name = "xkill",
        .capabilities = wf::CAPABILITY_GRAB_INPUT,
    };

    wf::key_callback on_activate = [=] (auto) -> bool
    {
        LOGI("xkill: activate binding fired (active=", active, ")");

        if (active)
        {
            return false;
        }

        if (!output->activate_plugin(&grab_interface))
        {
            return false;
        }

        active = true;
        input_grab->grab_input(wf::scene::layer::OVERLAY);
        wf::get_core().set_cursor("crosshair");
        return true;
    };

    /* Ends crosshair mode. If kill_it is true, whatever toplevel is
     * currently under the cursor gets killed. */
    void finish(bool kill_it)
    {
        if (!active)
        {
            return;
        }

        active = false;
        input_grab->ungrab_input();
        output->deactivate_plugin(&grab_interface);
        wf::get_core().set_cursor("default");

        if (kill_it)
        {
            kill_view_under_cursor();
        }
    }

    /* Finds the pid that actually owns the view's client connection. */
    static pid_t pid_for_view(wayfire_view view)
    {
        auto surface = view->get_wlr_surface();
        if (!surface)
        {
            return -1;
        }

#if WLR_HAS_XWAYLAND
        if (auto xw = wlr_xwayland_surface_try_from_wlr_surface(surface))
        {
            return xw->pid;
        }
#endif

        pid_t pid = -1;
        if (auto client = wl_resource_get_client(surface->resource))
        {
            wl_client_get_credentials(client, &pid, nullptr, nullptr);
        }

        return pid;
    }

    void kill_view_under_cursor()
    {
        auto view = wf::get_core().get_cursor_focus_view();
        if (!view || (view->role != wf::VIEW_ROLE_TOPLEVEL))
        {
            return;
        }

        auto pid = pid_for_view(view);
        if (pid <= 0)
        {
            LOGE("xkill: could not determine the pid of the clicked view");
            return;
        }

        LOGI("xkill: sending SIGKILL to pid ", pid);
        kill(pid, SIGKILL);
    }

  public:
    void init() override
    {
        input_grab = std::make_unique<wf::input_grab_t>("xkill", output, this, this, nullptr);
        output->add_key(activate_key, &on_activate);
        grab_interface.cancel = [=] () { finish(false); };
    }

    void handle_pointer_button(const wlr_pointer_button_event& event) override
    {
        if (event.state != WL_POINTER_BUTTON_STATE_PRESSED)
        {
            return;
        }

        // Left click kills, anything else (right click, middle click, ...)
        // just cancels.
        finish(event.button == BTN_LEFT);
    }

    void handle_pointer_motion(wf::pointf_t pointer_position, uint32_t time_ms) override
    {
        // Nothing to do — we only care about the click.
    }

    void handle_keyboard_key(wf::seat_t*, wlr_keyboard_key_event event) override
    {
        if ((event.state == WL_KEYBOARD_KEY_STATE_PRESSED) && (event.keycode == KEY_ESC))
        {
            finish(false);
        }
    }

    void fini() override
    {
        finish(false);
        output->rem_binding(&on_activate);
    }
};

DECLARE_WAYFIRE_PLUGIN(wf::per_output_plugin_t<wayfire_xkill_t>);
