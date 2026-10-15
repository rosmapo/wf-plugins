/*
 * rounded-corners — a Wayfire plugin that rounds off the corners of
 * toplevel windows, similar to how Hyprland's `decoration:rounding`
 * works (antialiased corner rounding applied as a post-process on the
 * window's own texture).
 *
 * Targets: Wayfire 0.9.0 (as shipped in Debian 13 "trixie").
 *
 * Implementation notes (this is the scene-graph based transformer API,
 * not the older wf::view_transformer_t from pre-0.8 tutorials):
 *
 *  - rounded_corners_node_t is a scene node (wf::scene::transformer_base_node_t)
 *    inserted into a view's transformer chain via
 *    view->get_transformed_node()->add_transformer(...).
 *  - It does not change the view's geometry at all, it only overrides
 *    get_bounding_box() (unchanged, passthrough) and generates a custom
 *    render_instance (rounded_corners_render_instance_t) that samples the
 *    view's own rendered texture through a small fragment shader which
 *    fades pixels to transparent outside a rounded-rectangle mask
 *    (antialiased with smoothstep, not a hard discard).
 *  - Fullscreen and maximized (TILED_EDGES_ALL) windows do not get
 *    rounded corners. The transformer is added/removed dynamically when
 *    those states change (view_fullscreen_signal / view_tiled_signal).
 *
 * Borders: when a window is decorated by Wayfire's `decoration` plugin
 * (border_size > 0), this plugin re-draws that border along the rounded
 * outline with a uniform thickness, using decoration/border_size and
 * decoration/active_color | inactive_color. Without this the square client
 * area would poke through the cut corners of the decoration frame. The ring
 * lies inside the window bounds, exactly where decoration draws its frame,
 * and it *replaces* the pixels there (no double-blending of translucent
 * colors).
 *
 * The rounding radius is exposed as the config option
 * `rounded-corners/radius`, described in rounded-corners.xml. That XML
 * file is what makes the option show up (with a slider) in WCM, and is
 * also what wayfire itself reads to know the option's type/default/range.
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
#include <wayfire/util/log.hpp>
#include <wayfire/config/types.hpp>
#include <glm/glm.hpp>
#include <algorithm>
#include <memory>

namespace
{
static const char *transformer_name = "rounded-corners";

const std::string vertex_source =
    R"(
#version 100
attribute mediump vec2 position;
varying mediump vec2 fposition;

uniform mat4 matrix;

void main() {
    gl_Position = matrix * vec4(position, 0.0, 1.0);
    fposition = position;
}
)";

/* Fades the sampled pixel to transparent once it is further than
 * `radius` from a corner, with a 1px antialiased smoothstep band
 * instead of a hard cutoff (closer to how Hyprland's rounding looks). */
const std::string frag_source =
    R"(
#version 100
@builtin_ext@

varying mediump vec2 fposition;
@builtin@

uniform mediump vec2 top_left;       // visible window rect (toplevel geometry)
uniform mediump vec2 bottom_right;
uniform mediump vec2 tex_top_left;   // rect covered by the texture (children bbox,
uniform mediump vec2 tex_bottom_right; // may be larger: CSD shadows etc.)
uniform mediump float radius;
uniform mediump float aa;            // size of one device pixel in logical units
uniform mediump float border_width;  // 0 = no border
uniform mediump vec4 border_color;   // non-premultiplied rgba

void main()
{
    mediump vec2 half_size = (bottom_right - top_left) * 0.5;
    mediump vec2 p = fposition - (top_left + half_size);
    mediump float r = min(radius, min(half_size.x, half_size.y));

    // Signed distance to the rounded rectangle: <0 inside, >0 outside.
    mediump vec2 q = abs(p) - half_size + vec2(r);
    mediump float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;

    // ~1px antialiased edge.
    mediump float mask = clamp(0.5 - d / aa, 0.0, 1.0);

    highp vec2 uv = (fposition - tex_top_left) / (tex_bottom_right - tex_top_left);
    uv.y = 1.0 - uv.y;

    mediump vec4 color = get_pixel(uv);

    if (border_width > 0.0)
    {
        // 1 in the band [-border_width, 0] below the edge, 0 further inside.
        mediump float cov = clamp((d + border_width) / aa + 0.5, 0.0, 1.0);
        // Premultiplied border color. It REPLACES the content here (decoration
        // draws the same color in the same place), so translucent colors are
        // not blended twice.
        mediump vec4 bc = vec4(border_color.rgb * border_color.a, border_color.a);
        color = mix(color, bc, cov);
    }

    // Premultiplied-alpha-safe: scaling color+alpha together keeps
    // blending correct against whatever is below.
    gl_FragColor = color * mask;
}
)";

// One option-wrapper instance for the whole plugin module; cheap to read
// on every frame.
wf::option_wrapper_t<int> o_radius{"rounded-corners/radius"};

/* Options owned by the decoration plugin. Looked up lazily as raw option
 * objects so that a missing decoration plugin/metadata can't crash us. */
template<class T>
std::shared_ptr<wf::config::option_t<T>> find_option(const std::string& name)
{
    return std::dynamic_pointer_cast<wf::config::option_t<T>>(
        wf::get_core().config.get_option(name));
}
}

class rounded_corners_render_instance_t;

/* The scene node itself. It has no children of its own -- its single
 * child is whatever was below it in the view's transformer chain
 * (either the next transformer, or the view's surface root node),
 * wired up automatically by transform_manager_node_t::add_transformer(). */
class rounded_corners_node_t : public wf::scene::transformer_base_node_t
{
  public:
    /* `view` is a raw pointer on purpose: the node is owned (indirectly) by
     * the view, so it never outlives it, and a shared_ptr would form a cycle. */
    explicit rounded_corners_node_t(wf::toplevel_view_interface_t *view) :
        wf::scene::transformer_base_node_t(false), view(view)
    {
        deco_border_size    = find_option<int>("decoration/border_size");
        deco_active_color   = find_option<wf::color_t>("decoration/active_color");
        deco_inactive_color = find_option<wf::color_t>("decoration/inactive_color");

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

    wf::geometry_t get_bounding_box() override
    {
        // We do not change the view's geometry at all.
        return get_children_bounding_box();
    }

    void gen_render_instances(std::vector<wf::scene::render_instance_uptr>& instances,
        wf::scene::damage_callback push_damage, wf::output_t *shown_on) override;

    /* Does the actual GL work: samples src_tex (the view's rendered
     * content) through the rounding shader and draws it into target,
     * restricted to damage. Called by rounded_corners_render_instance_t. */
    void paint(const wf::render_target_t& target, const wf::region_t& damage,
        wf::texture_t src_tex)
    {
        /* The texture covers the bounding box of everything the view draws,
         * which can be bigger than the actual window (client-side shadows,
         * invisible resize borders). The rounding and the border must follow
         * the real window rectangle -- the toplevel geometry, which already
         * includes the server-side decoration frame -- not the bbox. This is
         * the same idea soreau/filters uses with its "margins" uniform. */
        auto bbox = get_children_bounding_box();
        wf::geometry_t geo = bbox;
        if (view)
        {
            auto vg = view->get_geometry();
            if (vg.width > 0 && vg.height > 0)
            {
                geo = vg;
            }
        }

        float x = bbox.x, y = bbox.y, w = bbox.width, h = bbox.height;

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
        program.uniform2f("tex_top_left", x, y);
        program.uniform2f("tex_bottom_right", x + w, y + h);
        program.uniform1f("radius", (float)(int)o_radius);
        program.uniform1f("aa", 1.0f / target.scale);

        // Border = the decoration plugin's frame, if this view is decorated.
        float bw = 0.0f;
        glm::vec4 bcol{0.0f};
        if (view && view->should_be_decorated() && deco_border_size &&
            deco_active_color && deco_inactive_color)
        {
            bw = (float)std::max(0, deco_border_size->get_value());
            wf::color_t c = view->activated ?
                deco_active_color->get_value() : deco_inactive_color->get_value();
            bcol = glm::vec4(c.r, c.g, c.b, c.a);
        }

        program.uniform1f("border_width", bw);
        program.uniform4f("border_color", bcol);
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
    std::shared_ptr<wf::config::option_t<int>> deco_border_size;
    std::shared_ptr<wf::config::option_t<wf::color_t>> deco_active_color;
    std::shared_ptr<wf::config::option_t<wf::color_t>> deco_inactive_color;
};

class rounded_corners_render_instance_t :
    public wf::scene::transformer_render_instance_t<rounded_corners_node_t>
{
  public:
    using wf::scene::transformer_render_instance_t<rounded_corners_node_t>::transformer_render_instance_t;

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

/* Plugin glue: attaches/detaches the node on view map/unmap, and forces a
 * repaint of every window when the radius option changes (e.g. from WCM),
 * so moving the slider updates the effect immediately.
 *
 * Rounded corners are NOT applied to fullscreen or maximized (fully tiled)
 * windows. The transformer is added/removed dynamically when those states
 * change. */
class wayfire_rounded_corners_t : public wf::per_output_plugin_instance_t
{
  public:
    void init() override
    {
        output->connect(&on_view_mapped);
        output->connect(&on_view_fullscreen);
        output->connect(&on_view_tiled);

        auto damage_all = [=] ()
        {
            for (auto& v : wf::get_core().get_all_views())
            {
                v->damage();
            }
        };

        o_radius.set_callback(damage_all);
    }

    void fini() override
    {
        // Transformer nodes are torn down together with each view's own
        // scene-graph node, nothing to clean up here explicitly.
    }

  private:
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

        // Skip fullscreen windows.
        if (toplevel->pending_fullscreen())
        {
            return false;
        }

        // Skip any tiled window (maximized, or snapped to a half/quarter
        // of the screen via any single edge or combination of edges).
        if (toplevel->pending_tiled_edges() != 0)
        {
            return false;
        }

        return true;
    }

    /** Add the rounded-corners transformer if it is not already present. */
    void add_transformer(wayfire_view view)
    {
        auto toplevel = wf::toplevel_cast(view);
        if (!toplevel)
        {
            return;
        }

        auto& tr_manager = view->get_transformed_node();
        if (tr_manager->get_transformer(transformer_name))
        {
            return;
        }

        auto tr = std::make_shared<rounded_corners_node_t>(toplevel.get());
        tr_manager->add_transformer(tr, wf::TRANSFORMER_2D - 1, transformer_name);
        view->damage();
    }

    /** Remove the rounded-corners transformer if it is present. */
    void rem_transformer(wayfire_view view)
    {
        auto& tr_manager = view->get_transformed_node();
        if (!tr_manager->get_transformer(transformer_name))
        {
            return;
        }

        tr_manager->rem_transformer(transformer_name);
        view->damage();
    }

    /** Re-evaluate whether the view should have rounded corners and update. */
    void update_transformer(wayfire_view view)
    {
        if (should_round(view))
        {
            add_transformer(view);
        }
        else
        {
            rem_transformer(view);
        }
    }

    wf::signal::connection_t<wf::view_mapped_signal> on_view_mapped = [=] (wf::view_mapped_signal *ev)
    {
        auto view = ev->view;

        // Only consider normal application windows -- skip panels,
        // backgrounds, docks, etc.
        if (view->role != wf::VIEW_ROLE_TOPLEVEL)
        {
            return;
        }

        update_transformer(view);
        view->connect(&on_view_unmapped);
        view->connect(&on_view_activated);
    };

    // Focus changed: the border color depends on it, so repaint.
    wf::signal::connection_t<wf::view_activated_state_signal> on_view_activated =
        [=] (wf::view_activated_state_signal *ev)
    {
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
