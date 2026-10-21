/*
 * rounded-corners — Wayfire plugin: antialiased rounded corners that clip the
 * window itself (Hyprland-style `decoration:rounding`), plus soft shadows that
 * follow the rounded outline.
 *
 * Targets: Wayfire 0.9.0 (Debian 13 "trixie").
 *
 * The plugin is independent of the `decoration` plugin: it reads none of its
 * options and draws no border. Whatever frame/titlebar another plugin gives
 * the window is simply clipped together with the client.
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
 *   2. optionally draws a soft shadow *under* the window (never inside it) that
 *      follows the same rounded outline.
 *
 * The node's bounding box is enlarged by the shadow margin and damage is
 * enlarged the same way. Transformers are removed in fini().
 *
 * Do not run another shadow plugin (wayfire-shadows) together with the shadow
 * of this plugin; set shadow_radius = 0 if you prefer the other one.
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

    vec2 rel = abs(fposition - center);
    bool inside = (rel.x <= half_size.x) && (rel.y <= half_size.y);

    float d = sdf_rrect(fposition, center, half_size, r);

    // Window content (premultiplied). Transparent outside of the texture.
    vec4 color = vec4(0.0);
    highp vec2 uv = (fposition - tex_top_left) / (tex_bottom_right - tex_top_left);
    if (uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0)
    {
        uv.y = 1.0 - uv.y;
        color = get_pixel(uv);
    }

    // Coverage of the rounded window shape. Only meaningful inside the
    // rectangle; outside of it the window does not exist at all.
    float cov = inside ? clamp(0.5 - d / aa, 0.0, 1.0) : 0.0;

    // Corner mask: applied only in the four corner squares inside the window
    // rectangle. Straight edges and everything outside the rectangle are left
    // exactly as rendered.
    vec2 q = rel - half_size + vec2(r);
    float m = (inside && q.x > 0.0 && q.y > 0.0) ? cov : 1.0;

    vec4 result = color * m;

    if (shadow_radius > 0.0)
    {
        float ds = sdf_rrect(fposition - shadow_offset, center, half_size, r);
        float t = 1.0 - smoothstep(-shadow_radius, shadow_radius, ds);
        t *= (1.0 - cov); // never draw the shadow under the window
        vec4 sh = vec4(shadow_color.rgb * shadow_color.a, shadow_color.a) * t;
        result = result + sh * (1.0 - result.a);
    }

    gl_FragColor = result;
}
)";

wf::option_wrapper_t<int> o_radius{"rounded-corners/radius"};
wf::option_wrapper_t<int> o_shadow_radius{"rounded-corners/shadow_radius"};
wf::option_wrapper_t<int> o_shadow_x{"rounded-corners/shadow_offset_x"};
wf::option_wrapper_t<int> o_shadow_y{"rounded-corners/shadow_offset_y"};
wf::option_wrapper_t<wf::color_t> o_shadow_color{"rounded-corners/shadow_color"};
wf::option_wrapper_t<wf::color_t> o_shadow_color_inactive{"rounded-corners/inactive_shadow_color"};
wf::option_wrapper_t<bool> o_csd_shadow{"rounded-corners/shadow_on_client_decorated"};

/* Upper bound for how far a shadow can reach; used to damage generously when
 * options change or the node goes away. */
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

    /* Shadows are drawn for server-side decorated windows; client-decorated
     * windows usually bring their own shadow, so they only get ours when the
     * user asks for it. */
    bool shadow_enabled() const
    {
        if ((int)o_shadow_radius <= 0)
        {
            return false;
        }

        return (view && view->should_be_decorated()) || (bool)o_csd_shadow;
    }

    int get_margin() const
    {
        if (!shadow_enabled())
        {
            return 0;
        }

        int off = std::max(std::abs((int)o_shadow_x), std::abs((int)o_shadow_y));
        return (int)o_shadow_radius + off;
    }

    wf::geometry_t get_bounding_box() override
    {
        auto bb = get_children_bounding_box();
        int m   = get_margin();
        return {bb.x - m, bb.y - m, bb.width + 2 * m, bb.height + 2 * m};
    }

    /* Damage a generous area around the view. Used when options change or the
     * node is about to be removed, because the old extent of the shadow is not
     * known at that point. */
    void damage_extended()
    {
        auto bb = get_children_bounding_box();
        int m   = MAX_DAMAGE_MARGIN;
        wf::scene::damage_node(shared_from_this(),
            wf::geometry_t{bb.x - m, bb.y - m, bb.width + 2 * m, bb.height + 2 * m});
    }

    void gen_render_instances(std::vector<wf::scene::render_instance_uptr>& instances,
        wf::scene::damage_callback push_damage, wf::output_t *shown_on) override;

    void paint(const wf::render_target_t& target, const wf::region_t& damage,
        wf::texture_t src_tex)
    {
        /* The texture covers the bounding box of everything the view draws,
         * which can be bigger than the actual window (client-side shadows,
         * invisible resize borders). Rounding and shadow follow the
         * real window rectangle: the toplevel geometry, which includes the
         * server-side decoration frame. */
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

        // The quad we draw covers the texture plus the shadow margin.
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

        // Shadow.
        float sr = shadow_enabled() ? (float)(int)o_shadow_radius : 0.0f;
        wf::color_t sc = view && view->activated ?
            (wf::color_t)o_shadow_color : (wf::color_t)o_shadow_color_inactive;
        program.uniform1f("shadow_radius", sr);
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

    /* The shadow extends the area we draw to, so damage coming from the
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
        o_shadow_radius.set_callback(refresh_all_views);
        o_shadow_x.set_callback(refresh_all_views);
        o_shadow_y.set_callback(refresh_all_views);
        o_shadow_color.set_callback(refresh_all_views);
        o_shadow_color_inactive.set_callback(refresh_all_views);
        o_csd_shadow.set_callback(refresh_all_views);

        // Windows that already exist when the plugin is loaded.
        for (auto& v : output->wset()->get_views(wf::WSET_MAPPED_ONLY))
        {
            wayfire_view view = v;
            view->connect(&on_view_unmapped);
            view->connect(&on_view_activated);
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
    /** Re-damage every view that carries our transformer. */
    static void refresh_all_views()
    {
        for (auto& v : wf::get_core().get_all_views())
        {
            auto tr = v->get_transformed_node()->get_transformer<rounded_corners_node_t>(
                transformer_name);
            if (tr)
            {
                tr->damage_extended();
            }
        }
    }

    /** True when the view should receive rounded corners. */
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
        tr->damage_extended();
    }

    static void rem_transformer(wayfire_view view)
    {
        auto tr_manager = view->get_transformed_node();
        auto tr = tr_manager->get_transformer<rounded_corners_node_t>(transformer_name);
        if (!tr)
        {
            return;
        }

        // Repaint the area the shadow occupied, it is outside of the view's
        // bounding box once the node is gone.
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
    };

    // Focus changed: the shadow colour depends on it.
    wf::signal::connection_t<wf::view_activated_state_signal> on_view_activated =
        [=] (wf::view_activated_state_signal *ev)
    {
        // The view's bounding box already includes the shadow margin.
        ev->view->damage();
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
