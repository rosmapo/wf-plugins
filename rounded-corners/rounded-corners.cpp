/*
 * rounded-corners — Wayfire plugin that gives windows
 *
 *   - antialiased rounded corners that clip the window itself
 *     (Hyprland-style `decoration:rounding`),
 *   - an optional plain border (no titlebar, no buttons) that follows the
 *     rounded outline, with its own thickness and active / inactive colour,
 *   - optional soft shadows that follow the same outline.
 *
 * Targets: Wayfire 0.9.0 (Debian 13 "trixie").
 *
 * It is meant to REPLACE the stock `decoration` plugin: it reads none of its
 * options. Do not load both, or the window gets two frames.
 *
 * How it works
 * ------------
 * A transformer node is inserted into the view's transformer chain. It renders
 * the view into a temporary texture and redraws it through one fragment shader
 * which
 *
 *   1. cuts the four corners of the window rectangle (the toplevel geometry)
 *      with an antialiased signed-distance mask; only the corner squares inside
 *      the rectangle are touched, everything else passes through unchanged,
 *   2. draws the border OUTSIDE of that rectangle, between the window outline
 *      (radius r) and an outer outline (radius r + border width) that shares the
 *      same corner centres, so the thickness is uniform along the arcs and no
 *      window content is covered,
 *   3. draws a soft shadow under the outer outline (never under the window).
 *
 * Neither the border nor the shadow changes the window's real geometry (layout,
 * snapping, input); they are only painted. The node's bounding box grows by
 * border + shadow margin so there is room to paint, and damage grows with it.
 *
 * Server-side vs client-side decorated windows: whether a window is "client
 * decorated" is decided by core (xdg-decoration, core/preferred_decoration_mode)
 * and does not depend on any decoration plugin. Border and shadow are drawn for
 * server-side windows by default; client-decorated ones get them only when
 * border_on_client_decorated / shadow_on_client_decorated is on.
 *
 * Fullscreen and tiled (maximized / snapped) windows are left alone: no
 * rounding, no border, no shadow.
 */

#include <wayfire/view-transform.hpp>
#include <wayfire/view.hpp>
#include <wayfire/toplevel-view.hpp>
#include <wayfire/opengl.hpp>
#include <wayfire/plugin.hpp>
#include <wayfire/output.hpp>
#include <wayfire/core.hpp>
#include <wayfire/signal-definitions.hpp>
#include <wayfire/per-output-plugin.hpp>
#include <wayfire/option-wrapper.hpp>
#include <wayfire/workspace-set.hpp>
#include <wayfire/util/log.hpp>
#include <wayfire/config/types.hpp>
#include <glm/glm.hpp>
#include <algorithm>
#include <cstdlib>
#include <memory>

namespace
{
static const char *transformer_name = "rounded-corners";

const std::string vertex_source =
    R"(
#version 100
attribute highp vec2 position;
varying highp vec2 fposition;

uniform mat4 matrix;

void main() {
    gl_Position = matrix * vec4(position, 0.0, 1.0);
    fposition = position;
}
)";

const std::string frag_source =
    R"(
#version 100
@builtin_ext@

#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif

varying highp vec2 fposition;
@builtin@

uniform vec2 top_left;         // window rectangle (toplevel geometry)
uniform vec2 bottom_right;
uniform vec2 tex_top_left;     // rectangle covered by the texture (children bbox)
uniform vec2 tex_bottom_right;
uniform float radius;
uniform float aa;              // size of one device pixel in logical units
uniform float border_width;    // 0 = no border; drawn outside the window rect
uniform vec4 border_color;     // non-premultiplied rgba
uniform float shadow_radius;   // 0 = no shadow
uniform vec2 shadow_offset;
uniform vec4 shadow_color;     // non-premultiplied rgba

// Signed distance to a rounded rectangle: < 0 inside, > 0 outside.
float sdf_rrect(vec2 p, vec2 center, vec2 half_size, float r)
{
    vec2 q = abs(p - center) - half_size + vec2(r);
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

void main()
{
    vec2 center    = (top_left + bottom_right) * 0.5;
    vec2 half_size = (bottom_right - top_left) * 0.5;
    float r = min(radius, min(half_size.x, half_size.y));

    // Outer outline of the border. Same corner centres as the window outline:
    // half_out - r_out == half_size - r, so the ring has uniform thickness.
    float bw = border_width;
    vec2 half_out = half_size + vec2(bw);
    float r_out = (r > 0.0) ? r + bw : 0.0;

    vec2 rel = abs(fposition - center);
    bool inside = (rel.x <= half_size.x) && (rel.y <= half_size.y);

    float d_in  = sdf_rrect(fposition, center, half_size, r);
    float d_out = sdf_rrect(fposition, center, half_out, r_out);
    float cov_in  = clamp(0.5 - d_in / aa, 0.0, 1.0);
    float cov_out = clamp(0.5 - d_out / aa, 0.0, 1.0);

    // Window content (premultiplied). Transparent outside of the texture.
    vec4 color = vec4(0.0);
    highp vec2 uv = (fposition - tex_top_left) / (tex_bottom_right - tex_top_left);
    if (uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0)
    {
        uv.y = 1.0 - uv.y;
        color = get_pixel(uv);
    }

    // Border = outer shape minus window shape (never fills the window).
    vec4 border = vec4(0.0);
    if (bw > 0.0)
    {
        vec4 bc = vec4(border_color.rgb * border_color.a, border_color.a);
        border = bc * (cov_out * (1.0 - cov_in));
    }

    vec4 result;
    if (inside)
    {
        // Corner mask: only the four corner squares inside the rectangle are
        // cut; straight edges stay exactly as rendered.
        vec2 q = rel - half_size + vec2(r);
        float m = (q.x > 0.0 && q.y > 0.0) ? cov_in : 1.0;
        result = color * m + border;
    } else
    {
        // Outside the window rectangle: pass through whatever the client
        // painted there (e.g. its own shadow), the border goes over it.
        result = border + color * (1.0 - border.a);
    }

    if (shadow_radius > 0.0)
    {
        float ds = sdf_rrect(fposition - shadow_offset, center, half_out, r_out);
        float t = 1.0 - smoothstep(-shadow_radius, shadow_radius, ds);
        t *= (1.0 - cov_out); // never draw the shadow under window or border
        vec4 sh = vec4(shadow_color.rgb * shadow_color.a, shadow_color.a) * t;
        result = result + sh * (1.0 - result.a);
    }

    gl_FragColor = result;
}
)";

wf::option_wrapper_t<int> o_radius{"rounded-corners/radius"};

wf::option_wrapper_t<bool> o_border_enabled{"rounded-corners/border_enabled"};
wf::option_wrapper_t<int> o_border_size{"rounded-corners/border_size"};
wf::option_wrapper_t<wf::color_t> o_border_active{"rounded-corners/active_border_color"};
wf::option_wrapper_t<wf::color_t> o_border_inactive{"rounded-corners/inactive_border_color"};
wf::option_wrapper_t<bool> o_border_csd{"rounded-corners/border_on_client_decorated"};

wf::option_wrapper_t<bool> o_shadow_enabled{"rounded-corners/shadow_enabled"};
wf::option_wrapper_t<int> o_shadow_radius{"rounded-corners/shadow_radius"};
wf::option_wrapper_t<int> o_shadow_x{"rounded-corners/shadow_offset_x"};
wf::option_wrapper_t<int> o_shadow_y{"rounded-corners/shadow_offset_y"};
wf::option_wrapper_t<wf::color_t> o_shadow_color{"rounded-corners/shadow_color"};
wf::option_wrapper_t<wf::color_t> o_shadow_inactive{"rounded-corners/inactive_shadow_color"};
wf::option_wrapper_t<bool> o_shadow_csd{"rounded-corners/shadow_on_client_decorated"};

/* Upper bound for how far border + shadow can reach; used to damage generously
 * when options change or the node goes away. */
constexpr int MAX_DAMAGE_MARGIN = 512;
}

class rounded_corners_render_instance_t;

class rounded_corners_node_t : public wf::scene::transformer_base_node_t
{
  public:
    /* `view` is a raw pointer on purpose: the node is owned (indirectly) by
     * the view, so it never outlives it, and a shared_ptr would form a cycle. */
    explicit rounded_corners_node_t(wf::toplevel_view_interface_t *view) :
        wf::scene::transformer_base_node_t(false), view(view)
    {
        OpenGL::render_begin();
        program.compile(vertex_source, frag_source);
        OpenGL::render_end();
    }

    ~rounded_corners_node_t() override
    {
        OpenGL::render_begin();
        program.free_resources();
        OpenGL::render_end();
    }

    std::string stringify() const override
    {
        return "rounded-corners";
    }

    /* Server-side decorated, as decided by core (not by any plugin). */
    bool is_server_side() const
    {
        return view && view->should_be_decorated();
    }

    /** Border thickness in logical pixels, 0 when no border is drawn. */
    int border_width() const
    {
        if (!(bool)o_border_enabled || (!is_server_side() && !(bool)o_border_csd))
        {
            return 0;
        }

        return std::max(0, (int)o_border_size);
    }

    /** Shadow blur radius in logical pixels, 0 when no shadow is drawn. */
    int shadow_radius() const
    {
        if (!(bool)o_shadow_enabled || (!is_server_side() && !(bool)o_shadow_csd))
        {
            return 0;
        }

        return std::max(0, (int)o_shadow_radius);
    }

    /** How far beyond the window rectangle we paint. */
    int get_margin() const
    {
        int m  = border_width();
        int sr = shadow_radius();
        if (sr > 0)
        {
            m += sr + std::max(std::abs((int)o_shadow_x), std::abs((int)o_shadow_y));
        }

        return m;
    }

    wf::geometry_t get_bounding_box() override
    {
        auto bb = get_children_bounding_box();
        int m   = get_margin();
        return {bb.x - m, bb.y - m, bb.width + 2 * m, bb.height + 2 * m};
    }

    /* Damage a generous area around the view. Used when options change or the
     * node is about to be removed, because the old extent of border and shadow
     * is not known at that point. */
    void damage_extended()
    {
        auto bb = get_children_bounding_box();
        int m   = MAX_DAMAGE_MARGIN;
        wf::scene::damage_node(shared_from_this(),
            wf::geometry_t{bb.x - m, bb.y - m, bb.width + 2 * m, bb.height + 2 * m});
    }

    /* The margin may have changed (option toggled, SSD <-> CSD): tell the scene
     * and repaint the old and new area. */
    void geometry_changed()
    {
        damage_extended();
        wf::scene::update(shared_from_this(), wf::scene::update_flag::GEOMETRY);
    }

    void gen_render_instances(std::vector<wf::scene::render_instance_uptr>& instances,
        wf::scene::damage_callback push_damage, wf::output_t *shown_on) override;

    void paint(const wf::render_target_t& target, const wf::region_t& damage,
        wf::texture_t src_tex)
    {
        /* The texture covers the bounding box of everything the view draws,
         * which can be bigger than the actual window (client-side shadows,
         * invisible resize borders). Rounding, border and shadow follow the
         * real window rectangle: the toplevel geometry. */
        auto tex_box = get_children_bounding_box();
        wf::geometry_t geo = tex_box;
        if (view)
        {
            auto vg = view->get_geometry();
            if (vg.width > 0 && vg.height > 0)
            {
                geo = vg;
            }
        }

        // The quad we draw covers the texture plus the border/shadow margin.
        auto quad = get_bounding_box();
        float x = quad.x, y = quad.y, w = quad.width, h = quad.height;

        OpenGL::render_begin(target);

        program.use(src_tex.type);
        program.set_active_texture(src_tex);

        vertex_data = {
            x, y + h,
            x + w, y + h,
            x + w, y,
            x, y,
        };
        program.attrib_pointer("position", 2, 0, vertex_data.data(), GL_FLOAT);
        program.uniform2f("top_left", geo.x, geo.y);
        program.uniform2f("bottom_right", geo.x + geo.width, geo.y + geo.height);
        program.uniform2f("tex_top_left", tex_box.x, tex_box.y);
        program.uniform2f("tex_bottom_right",
            tex_box.x + tex_box.width, tex_box.y + tex_box.height);
        program.uniform1f("radius", (float)std::max(0, (int)o_radius));
        program.uniform1f("aa", 1.0f / target.scale);

        bool active = view && view->activated;

        // Border.
        wf::color_t bc = active ? (wf::color_t)o_border_active : (wf::color_t)o_border_inactive;
        program.uniform1f("border_width", (float)border_width());
        program.uniform4f("border_color", glm::vec4(bc.r, bc.g, bc.b, bc.a));

        // Shadow.
        wf::color_t sc = active ? (wf::color_t)o_shadow_color : (wf::color_t)o_shadow_inactive;
        program.uniform1f("shadow_radius", (float)shadow_radius());
        program.uniform2f("shadow_offset", (float)(int)o_shadow_x, (float)(int)o_shadow_y);
        program.uniform4f("shadow_color", glm::vec4(sc.r, sc.g, sc.b, sc.a));

        program.uniformMatrix4f("matrix", target.get_orthographic_projection());

        GL_CALL(glEnable(GL_BLEND));
        GL_CALL(glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA));

        for (const auto& box : damage)
        {
            target.logic_scissor(wlr_box_from_pixman_box(box));
            GL_CALL(glDrawArrays(GL_TRIANGLE_FAN, 0, 4));
        }

        GL_CALL(glDisable(GL_BLEND));
        program.deactivate();
        OpenGL::render_end();
    }

  private:
    OpenGL::program_t program;
    std::vector<GLfloat> vertex_data;
    wf::toplevel_view_interface_t *view;
};

class rounded_corners_render_instance_t :
    public wf::scene::transformer_render_instance_t<rounded_corners_node_t>
{
  public:
    using wf::scene::transformer_render_instance_t<rounded_corners_node_t>::transformer_render_instance_t;

    /* Border and shadow extend the area we draw to, so damage coming from the
     * children has to grow by the same margin. */
    void transform_damage_region(wf::region_t& damage) override
    {
        int m = self->get_margin();
        if (m > 0)
        {
            damage.expand_edges(m);
        }
    }

    void render(const wf::render_target_t& target, const wf::region_t& damage) override
    {
        auto src_tex = get_texture(target.scale);
        self->paint(target, damage, src_tex);
    }
};

void rounded_corners_node_t::gen_render_instances(
    std::vector<wf::scene::render_instance_uptr>& instances,
    wf::scene::damage_callback push_damage, wf::output_t *shown_on)
{
    instances.push_back(
        std::make_unique<rounded_corners_render_instance_t>(this, push_damage, shown_on));
}

/* Plugin glue. */
class wayfire_rounded_corners_t : public wf::per_output_plugin_instance_t
{
  public:
    void init() override
    {
        output->connect(&on_view_mapped);
        output->connect(&on_view_fullscreen);
        output->connect(&on_view_tiled);

        o_radius.set_callback(refresh_all_views);
        o_border_enabled.set_callback(refresh_all_views);
        o_border_size.set_callback(refresh_all_views);
        o_border_active.set_callback(refresh_all_views);
        o_border_inactive.set_callback(refresh_all_views);
        o_border_csd.set_callback(refresh_all_views);
        o_shadow_enabled.set_callback(refresh_all_views);
        o_shadow_radius.set_callback(refresh_all_views);
        o_shadow_x.set_callback(refresh_all_views);
        o_shadow_y.set_callback(refresh_all_views);
        o_shadow_color.set_callback(refresh_all_views);
        o_shadow_inactive.set_callback(refresh_all_views);
        o_shadow_csd.set_callback(refresh_all_views);

        // Windows that already exist when the plugin is loaded.
        for (auto& v : output->wset()->get_views(wf::WSET_MAPPED_ONLY))
        {
            wayfire_view view = v;
            view->connect(&on_view_unmapped);
            view->connect(&on_view_activated);
            view->connect(&on_view_decoration_state);
            update_transformer(view);
        }
    }

    void fini() override
    {
        // The nodes contain code from this shared object; they must not
        // outlive it.
        for (auto& v : wf::get_core().get_all_views())
        {
            rem_transformer(v);
        }
    }

  private:
    /** Re-evaluate size/margins and repaint every view that carries our node. */
    static void refresh_all_views()
    {
        for (auto& v : wf::get_core().get_all_views())
        {
            auto tr = v->get_transformed_node()->get_transformer<rounded_corners_node_t>(
                transformer_name);
            if (tr)
            {
                tr->geometry_changed();
            }
        }
    }

    /** True when the view should receive rounded corners, border and shadow. */
    static bool should_round(wayfire_view view)
    {
        if (view->role != wf::VIEW_ROLE_TOPLEVEL)
        {
            return false;
        }

        auto toplevel = wf::toplevel_cast(view);
        if (!toplevel)
        {
            return false;
        }

        if (toplevel->pending_fullscreen())
        {
            return false;
        }

        // Skip any tiled window (maximized, or snapped to an edge/corner).
        if (toplevel->pending_tiled_edges() != 0)
        {
            return false;
        }

        return true;
    }

    static void add_transformer(wayfire_view view)
    {
        auto toplevel = wf::toplevel_cast(view);
        if (!toplevel)
        {
            return;
        }

        auto tr_manager = view->get_transformed_node();
        if (tr_manager->get_transformer(transformer_name))
        {
            return;
        }

        auto tr = std::make_shared<rounded_corners_node_t>(toplevel.get());
        tr_manager->add_transformer(tr, wf::TRANSFORMER_2D - 1, transformer_name);
        tr->geometry_changed();
    }

    static void rem_transformer(wayfire_view view)
    {
        auto tr_manager = view->get_transformed_node();
        auto tr = tr_manager->get_transformer<rounded_corners_node_t>(transformer_name);
        if (!tr)
        {
            return;
        }

        // Repaint the area border and shadow occupied; it is outside of the
        // view's bounding box once the node is gone.
        tr->damage_extended();
        tr_manager->rem_transformer(transformer_name);
        view->damage();
    }

    static void update_transformer(wayfire_view view)
    {
        if (should_round(view))
        {
            add_transformer(view);
        } else
        {
            rem_transformer(view);
        }
    }

    wf::signal::connection_t<wf::view_mapped_signal> on_view_mapped = [=] (wf::view_mapped_signal *ev)
    {
        auto view = ev->view;

        // Only normal application windows, not panels, backgrounds, docks...
        if (view->role != wf::VIEW_ROLE_TOPLEVEL)
        {
            return;
        }

        update_transformer(view);
        view->connect(&on_view_unmapped);
        view->connect(&on_view_activated);
        view->connect(&on_view_decoration_state);
    };

    // Focus changed: border and shadow colour depend on it. The view's
    // bounding box already includes the margin, so a plain damage is enough.
    wf::signal::connection_t<wf::view_activated_state_signal> on_view_activated =
        [=] (wf::view_activated_state_signal *ev)
    {
        ev->view->damage();
    };

    // The client switched between server-side and client-side decorations:
    // border and shadow may appear or disappear, so the margin changes.
    wf::signal::connection_t<wf::view_decoration_state_updated_signal> on_view_decoration_state =
        [=] (wf::view_decoration_state_updated_signal *ev)
    {
        wayfire_view view = ev->view;
        auto tr = view->get_transformed_node()->get_transformer<rounded_corners_node_t>(
            transformer_name);
        if (tr)
        {
            tr->geometry_changed();
        }
    };

    wf::signal::connection_t<wf::view_unmapped_signal> on_view_unmapped =
        [=] (wf::view_unmapped_signal *ev)
    {
        rem_transformer(ev->view);
    };

    wf::signal::connection_t<wf::view_fullscreen_signal> on_view_fullscreen =
        [=] (wf::view_fullscreen_signal *ev)
    {
        update_transformer(ev->view);
    };

    wf::signal::connection_t<wf::view_tiled_signal> on_view_tiled =
        [=] (wf::view_tiled_signal *ev)
    {
        update_transformer(ev->view);
    };
};

DECLARE_WAYFIRE_PLUGIN(wf::per_output_plugin_t<wayfire_rounded_corners_t>);
