#pragma once

#include <random>

#include <cgv/base/node.h>
#include <cgv/gui/event_handler.h>
#include <cgv/gui/provider.h>
#include <cgv/render/drawable.h>
#include <cgv/render/render_types.h>
#include <cgv_gl/volume_renderer.h>
#include <cgv_app/application_plugin.h>
#include <cgv_gl/box_wire_render_data.h>
#include <cgv/render/color_map.h>
#include <cgv_app/color_map_editor.h>
#include <cgv_app/color_map_legend.h>
#include <cgv/render/managed_frame_buffer.h>

class slice_renderer :
	public cgv::app::application_plugin // inherit from application plugin to enable overlay support
{
private:
	bool do_calculate_gradients;

protected:
	/// store a pointer to the color map editor overlay which is used to edit the volume transfer function
	cgv::app::color_map_editor_ptr transfer_function_editor_ptr;
	cgv::app::color_map_legend_ptr transfer_function_legend_ptr;
		
	/// resolution of the volume
	uvec3 vres;
	/// spacing of the voxels
	vec3 vspacing;
	/// whether to show bounding box
	bool show_box;

	// Information needed to store the next screenshot to disk
	bool store_next_screenshot;
	std::string screenshot_filename;

	// Framebuffer for volume
	cgv::render::managed_frame_buffer volume_frame_buffer;
	

	// Volume data
	std::vector<float> vol_data;
	box3 volume_bounding_box;
	cgv::render::texture volume_tex;
	
	
	// Render members
	/// store a pointer to the view
	cgv::render::view* view_ptr;
	/// render style for volume
	cgv::render::volume_render_style vstyle;
	/// index of the transfer function preset
	cgv::type::DummyEnum transfer_function_preset_idx = (cgv::type::DummyEnum)1;
	/// using a color map to define the volume transfer function
	/// gl_color_map supports generation of a texture from its contents
	cgv::render::gl_color_map transfer_function;
	/// render data for wireframe box
	cgv::render::box_wire_render_data<> box_rd;
	
	void handle_transfer_function_change();

	void update_bounding_box();

	void load_transfer_function_preset();

	void create_volume(cgv::render::context& ctx);
	void splat_spheres(std::vector<float>& vol_data, float voxel_size, std::mt19937& rng, size_t n, float radius, float contribution);
	void splat_sphere(std::vector<float>& vol_data, float voxel_size, const vec3& pos, float radius, float contribution);

	void load_volume_from_file(const std::string& file_name);

	void fit_to_resolution();
	void fit_to_spacing();
	void fit_to_resolution_and_spacing();

	void create_histogram();

	// Have a function allowing to resize our render target
	void resize_render_target(int width, int height);

	void save_buffer_to_file(cgv::render::context& ctx);
public:
	// default constructor
	slice_renderer();
	
	// interface of cgv::base::node
	std::string get_type_name() const { return "slice_renderer"; }
	void stream_stats(std::ostream& os);
	void on_set(void* member_ptr);
	bool self_reflect(cgv::reflect::reflection_handler& _rh);

	// interface of cgv::gui::event_handler
	void stream_help(std::ostream& os);
	bool handle_event(cgv::gui::event& e);

	// interface of cgv::render::drawable
	void clear(cgv::render::context& ctx);
	bool init(cgv::render::context& ctx);
	void init_frame(cgv::render::context& ctx);
	void draw(cgv::render::context& ctx);
	void after_finish(cgv::render::context&) override;

	// interface of provider
	void create_gui();
};