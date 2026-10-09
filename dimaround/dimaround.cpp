// dimaround - Hyprland-like "dimaround" for Wayfire 0.9.x
// Fixed v4: Connect to output_plugin_activated_changed_signal on EACH OUTPUT
#include <algorithm>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include <wayfire/plugin.hpp>
#include <wayfire/core.hpp>
#include <wayfire/seat.hpp>
#include <wayfire/output.hpp>
#include <wayfire/output-layout.hpp>
#include <wayfire/view.hpp>
#include <wayfire/toplevel-view.hpp>
#include <wayfire/scene.hpp>
#include <wayfire/scene-operations.hpp>
#include <wayfire/scene-render.hpp>
#include <wayfire/render-manager.hpp>
#include <wayfire/opengl.hpp>
#include <wayfire/signal-definitions.hpp>
#include <wayfire/option-wrapper.hpp>
#include <wayfire/util.hpp>
#include <wayfire/debug.hpp>

namespace
{
class dim_node_t : public wf::scene::node_t
{
public:
    wf::output_t *output = nullptr;
    double alpha = 0.5;
    bool blocked = false;

    dim_node_t() : node_t(false) {}

    void gen_render_instances(
        std::vector<wf::scene::render_instance_uptr>& instances,
        wf::scene::damage_callback push_damage, wf::output_t *shown_on) override;

    wf::geometry_t get_bounding_box() override
    {
        return {-100000, -100000, 200000, 200000};
    }

    std::string stringify() const override { return "dimaround"; }
};

class dim_render_instance_t : public wf::scene::simple_render_instance_t<dim_node_t>
{
public:
    using simple_render_instance_t::simple_render_instance_t;

    void render(const wf::render_target_t& target, const wf::region_t& region) override
    {
        if (self->blocked)
            return;

        OpenGL::render_begin(target);
        GL_CALL(glEnable(GL_BLEND));
        GL_CALL(glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA));

        for (const auto& box : region)
        {
            target.logic_scissor(wlr_box_from_pixman_box(box));
            OpenGL::render_rectangle(self->get_bounding_box(),
                wf::color_t{0.0, 0.0, 0.0, self->alpha},
                target.get_orthographic_projection());
        }
        OpenGL::render_end();
    }
};

void dim_node_t::gen_render_instances(
    std::vector<wf::scene::render_instance_uptr>& instances,
    wf::scene::damage_callback push_damage, wf::output_t *shown_on)
{
    if (blocked || !output || shown_on != output)
        return;

    instances.push_back(
        std::make_unique<dim_render_instance_t>(this, push_damage, shown_on));
}
} // namespace

class wayfire_dimaround : public wf::plugin_interface_t
{
    std::shared_ptr<dim_node_t> dim;
    wf::wl_idle_call idle;

    wf::option_wrapper_t<double>      opacity{"dimaround/opacity"};
    wf::option_wrapper_t<std::string> app_ids{"dimaround/app_ids"};
    wf::option_wrapper_t<std::string> disabled_during{"dimaround/disabled_during"};

    std::unordered_set<std::string> active_blockers;

    // Store connections for each output dynamically
    std::map<wf::output_t*, std::unique_ptr<wf::signal::connection_t<wf::output_plugin_activated_changed_signal>>> output_conns;

    std::unordered_set<std::string> parse_disabled()
    {
        std::unordered_set<std::string> s;
        std::string list = disabled_during;
        std::replace(list.begin(), list.end(), ',', ' ');
        std::istringstream ss(list);
        std::string name;
        while (ss >> name)
            s.insert(name);
        return s;
    }

    bool is_blocked() const
    {
        return !active_blockers.empty();
    }

    bool any_output_has_blocker()
    {
        auto disabled = parse_disabled();
        for (auto *o : wf::get_core().output_layout->get_outputs())
        {
            for (const auto& name : disabled)
            {
                if (o->is_plugin_active(name))
                    return true;
            }
        }
        return false;
    }

    void set_blocked(bool blocked)
    {
        dim->blocked = blocked;
        if (blocked)
        {
            if (dim->parent())
            {
                wf::scene::damage_node(dim, dim->get_bounding_box());
                wf::scene::remove_child(dim);
            }
            dim->output = nullptr;
        }
    }

    void schedule_refresh()
    {
        idle.run_once([=] () { refresh(); });
    }

    // ========== KEY: Connect to EACH OUTPUT individually ==========
    void connect_output(wf::output_t *output)
    {
        auto conn = std::make_unique<wf::signal::connection_t<wf::output_plugin_activated_changed_signal>>(
            [=] (wf::output_plugin_activated_changed_signal *ev)
            {
                if (!ev) return;

                auto disabled = parse_disabled();
                if (!disabled.count(ev->plugin_name))
                    return;

                if (ev->activated)
                    active_blockers.insert(ev->plugin_name);
                else
                    active_blockers.erase(ev->plugin_name);

                bool blocked = is_blocked() || any_output_has_blocker();
                set_blocked(blocked);
                
                if (!blocked)
                    schedule_refresh();
            }
        );
        
        output->connect(conn.get());
        output_conns[output] = std::move(conn);
    }

    void disconnect_output(wf::output_t *output)
    {
        output_conns.erase(output);
    }

    wf::signal::connection_t<wf::output_added_signal> on_output_added =
        [=] (wf::output_added_signal *ev) { connect_output(ev->output); };

    wf::signal::connection_t<wf::output_removed_signal> on_output_removed =
        [=] (wf::output_removed_signal *ev) { disconnect_output(ev->output); };

    // ordinary view signals (these are correctly emitted on core)
    wf::signal::connection_t<wf::keyboard_focus_changed_signal> on_focus =
        [=] (auto) { schedule_refresh(); };
    wf::signal::connection_t<wf::view_mapped_signal> on_mapped =
        [=] (auto) { schedule_refresh(); };
    wf::signal::connection_t<wf::view_unmapped_signal> on_unmapped =
        [=] (auto) { schedule_refresh(); };
    wf::signal::connection_t<wf::view_tiled_signal> on_tiled =
        [=] (auto) { schedule_refresh(); };
    wf::signal::connection_t<wf::view_fullscreen_signal> on_fullscreen =
        [=] (auto) { schedule_refresh(); };
    wf::signal::connection_t<wf::view_minimized_signal> on_minimized =
        [=] (auto) { schedule_refresh(); };
    wf::signal::connection_t<wf::view_set_output_signal> on_set_output =
        [=] (auto) { schedule_refresh(); };

    static bool is_floating(wayfire_toplevel_view view)
    {
        auto st = view->toplevel()->current();
        return st.tiled_edges == 0 && !st.fullscreen;
    }

    bool matches(wayfire_toplevel_view view)
    {
        std::string list = app_ids;
        std::replace(list.begin(), list.end(), ',', ' ');
        std::istringstream ss(list);
        std::string tok;
        while (ss >> tok)
            if (tok == "*" || tok == view->get_app_id())
                return true;
        return false;
    }

    void detach()
    {
        if (dim->parent())
        {
            wf::scene::damage_node(dim, dim->get_bounding_box());
            wf::scene::remove_child(dim);
        }
        dim->output = nullptr;
    }

    void attach(wayfire_toplevel_view view)
    {
        if (dim->blocked || is_blocked() || any_output_has_blocker())
        {
            set_blocked(true);
            return;
        }

        detach();

        auto root   = view->get_root_node();
        auto parent = dynamic_cast<wf::scene::floating_inner_node_t*>(root->parent());
        if (!parent)
            return;

        auto kids = parent->get_children();
        auto it = std::find_if(kids.begin(), kids.end(),
            [&](const wf::scene::node_ptr& n) { return n.get() == root.get(); });
        if (it == kids.end())
            return;

        dim->output  = view->get_output();
        dim->alpha   = std::clamp((double)opacity, 0.0, 1.0);
        dim->blocked = false;

        kids.insert(std::next(it), dim);
        parent->set_children_list(kids);
        wf::scene::update(parent->shared_from_this(),
            wf::scene::update_flag::CHILDREN_LIST);
        wf::scene::damage_node(dim, dim->get_bounding_box());
    }

    void refresh()
    {
        bool blocked = is_blocked() || any_output_has_blocker();
        set_blocked(blocked);
        if (blocked)
            return;

        auto active = wf::get_core().seat->get_active_view();
        auto view   = active ? wf::toplevel_cast(active) : nullptr;

        if (!view || !view->get_output() || !view->is_mapped() ||
            view->minimized || !is_floating(view) || !matches(view))
        {
            detach();
            return;
        }

        attach(view);
    }

public:
    void init() override
    {
        dim = std::make_shared<dim_node_t>();

        // 1. Track outputs and connect to their specific plugin activation signals
        wf::get_core().connect(&on_output_added);
        wf::get_core().connect(&on_output_removed);
        for (auto *o : wf::get_core().output_layout->get_outputs()) {
            connect_output(o);
        }

        // 2. Connect view signals to core
        wf::get_core().connect(&on_focus);
        wf::get_core().connect(&on_mapped);
        wf::get_core().connect(&on_unmapped);
        wf::get_core().connect(&on_tiled);
        wf::get_core().connect(&on_fullscreen);
        wf::get_core().connect(&on_minimized);
        wf::get_core().connect(&on_set_output);

        disabled_during.set_callback([=] () {
            auto disabled = parse_disabled();
            for (auto it = active_blockers.begin(); it != active_blockers.end(); )
            {
                if (!disabled.count(*it))
                    it = active_blockers.erase(it);
                else
                    ++it;
            }
            bool blocked = is_blocked() || any_output_has_blocker();
            set_blocked(blocked);
            if (!blocked)
                schedule_refresh();
        });

        opacity.set_callback([=] () { schedule_refresh(); });
        app_ids.set_callback([=] () { schedule_refresh(); });

        schedule_refresh();
    }

    void fini() override
    {
        output_conns.clear(); 
        idle.disconnect();
        set_blocked(true);
        detach();
    }

    bool is_unloadable() override { return true; }
};

DECLARE_WAYFIRE_PLUGIN(wayfire_dimaround);