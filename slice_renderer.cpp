#include "slice_renderer.h"

#include <chrono>
#include <filesystem>
#include <cgv/defines/quote.h>
#include <cgv/gui/trigger.h>
#include <cgv/gui/key_event.h>
#include <cgv/gui/mouse_event.h>
#include <cgv/math/ftransform.h>
#include <cgv/reflect/reflect_enum.h>
#include <cgv/signal/rebind.h>
#include <cgv/utils/advanced_scan.h>
#include <cgv/utils/file.h>
#include <cgv/utils/big_binary_file.h>
#include <cgv_gl/gl/gl.h>
#include <cgv_gl/gl/gl_tools.h>
#include "cgv/math/constants.h"


#include "cgv/media/image/image_writer.h"

#include <fstream>

#include "fpng.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace cgv {
	namespace reflect {
	}
}

slice_renderer::slice_renderer() : application_plugin("Slice Renderer")
{
	// setup volume bounding box as unit cube centered around origin
	volume_bounding_box = box3(vec3(-0.5f), vec3(0.5f));

	store_next_screenshot = false;

	volume_frame_buffer.add_attachment("COLOR", "uint8[R,G,B,A]");

	// Initialize random number generator
	rng = std::mt19937(std::chrono::system_clock::now().time_since_epoch().count());
	dist = std::uniform_real_distribution<float>(0.0f, 1.0f);
	

	// configure texture format, filtering and wrapping (no context necessary)
	volume_tex = cgv::render::texture("flt32[R]");
	volume_tex.set_min_filter(cgv::render::TF_LINEAR);
	volume_tex.set_mag_filter(cgv::render::TF_LINEAR);
	volume_tex.set_wrap_s(cgv::render::TW_CLAMP_TO_BORDER);
	volume_tex.set_wrap_t(cgv::render::TW_CLAMP_TO_BORDER);
	volume_tex.set_wrap_r(cgv::render::TW_CLAMP_TO_BORDER);
	volume_tex.set_border_color(0.0f, 0.0f, 0.0f, 0.0f);

	vstyle.enable_depth_test = false;

	show_box = false;

	sample_count = 150;

	randomize_zoom = false;
	randomize_offset = false;

	sample_width = 1024;
	sample_height = 1024;
	
	
	vres = uvec3(128);
	vspacing = vec3(1.0f);

	view_ptr = nullptr;

	// instantiate a color map editor as an overlay for this viewer
	transfer_function_editor_ptr = register_overlay<cgv::app::color_map_editor>("Editor");
	transfer_function_editor_ptr->gui_options.show_heading = false;
	// enable support for editing opacity values
	transfer_function_editor_ptr->set_opacity_support(true);
	// connect a callback function to handle changes of the transfer function
	transfer_function_editor_ptr->set_on_change_callback(std::bind(&slice_renderer::handle_transfer_function_change, this));
	transfer_function_editor_ptr->set_visibility(false);
	
	// instantiate a color map legend to show the used transfer function
	transfer_function_legend_ptr = register_overlay<cgv::app::color_map_legend>("Legend");
	transfer_function_legend_ptr->set_title("Density");
	transfer_function_legend_ptr->set_visibility(false);

	fpng::fpng_init();
}

void slice_renderer::stream_stats(std::ostream& os)
{
	os << "slice_renderer: resolution=" << vres[0] << "x" << vres[1] << "x" << vres[2] << std::endl;
}

bool slice_renderer::self_reflect(cgv::reflect::reflection_handler& rh)
{
	return rh.reflect_member("show_box", show_box) &&
		rh.reflect_member("sample_count", sample_count) &&
		rh.reflect_member("randomize_zoom", randomize_zoom) &&
		rh.reflect_member("randomize_offset", randomize_offset) &&
		rh.reflect_member("sample_width", sample_width) &&
		rh.reflect_member("sample_height", sample_height);
			
}

void slice_renderer::stream_help(std::ostream& os) 
{
	os << "slice_renderer: toggle <B>ox\n, toggle <T>ransfer function editor, ctrl+click in transfer function editor to add points, alt+click to remove";
}

bool slice_renderer::handle_event(cgv::gui::event& e) 
{

	if(e.get_kind() == cgv::gui::EID_MOUSE) {
		auto& me = static_cast<cgv::gui::mouse_event&>(e);

		if(me.get_flags() & cgv::gui::EF_DND) {
			switch(me.get_action()) {
			case cgv::gui::MA_ENTER:
				return true;
			case cgv::gui::MA_DRAG:
				return true;
			case cgv::gui::MA_LEAVE:
				return true;
			case cgv::gui::MA_RELEASE:
				load_volume_from_file(me.get_dnd_text());
				return true;
			default: break;
			}
		}
	} else if(e.get_kind() == cgv::gui::EID_KEY) {
		auto& ke = static_cast<cgv::gui::key_event&>(e);
		if(ke.get_action() == cgv::gui::KA_RELEASE)
			return false;

		switch(ke.get_key()) {
		case 'B':
			show_box = !show_box;
			on_set(&show_box);
			return true;
		case 'T':
			if(transfer_function_editor_ptr) {
				transfer_function_editor_ptr->set_visibility(!transfer_function_editor_ptr->is_visible());
				post_redraw();
			}
			return true;
		// When pressing I, we want to resize the application to 512x512
		case 'I':
			sample_width = 512;
			sample_height = 512;
			update_member(&sample_width);
			update_member(&sample_height);
			resize_render_target();
			return true;
		// When pressing O, we want to resize the application to 1024x1024
		case 'O':
			sample_width = 1024;
			sample_height = 1024;
			update_member(&sample_width);
			update_member(&sample_height);
			resize_render_target();
			return true;
		// When pressing S, we want to output a copy of the current frame to a file
		case 'P':
			// Set the flag that we want to generate a screenshot
			store_next_screenshot = true;
			// As a filename, just call it output.png
			screenshot_filename = "output.tiff";
			return true;
		case 'S':
			dump_image_to_path("output.png");
			return true;
			
		default: break;
		}
	}

	return false;
}

void slice_renderer::on_set(void* member_ptr) 
{
	vec3& a = volume_bounding_box.ref_min_pnt();
	vec3& b = volume_bounding_box.ref_max_pnt();

	if (member_ptr == &a[0] ||
		member_ptr == &a[1] ||
		member_ptr == &a[2] ||
		member_ptr == &b[0] ||
		member_ptr == &b[1] ||
		member_ptr == &b[2]) {
		update_bounding_box();
	}

	if(member_ptr == &transfer_function_preset_idx)
		load_transfer_function_preset();

	update_member(member_ptr);
	post_redraw();
}

void slice_renderer::clear(cgv::render::context& ctx)
{
	cgv::render::ref_volume_renderer(ctx, -1);
	cgv::render::ref_box_wire_renderer(ctx, -1);

	volume_frame_buffer.clear(ctx);
}

bool slice_renderer::init(cgv::render::context& ctx)
{
	cgv::render::ref_volume_renderer(ctx, 1);
	cgv::render::ref_box_wire_renderer(ctx, 1);

	// init the box wire render data object
	box_rd.init(ctx);
	// add the volume bounding box
	box_rd.add(volume_bounding_box.get_center(), volume_bounding_box.get_extent());

	// init a color map used as a transfer function
	transfer_function.init(ctx);
	load_transfer_function_preset();

	create_volume(ctx);
	
	return true;
}

void slice_renderer::init_frame(cgv::render::context& ctx) {
	if(!view_ptr) {
		view_ptr = find_view_as_node();
		
		if(view_ptr) {
			// do one-time initialization
			// set the transfer function as the to-be-edited color map in the editor
			if(transfer_function_editor_ptr)
				transfer_function_editor_ptr->set_color_map(&transfer_function);
			if(transfer_function_legend_ptr)
				transfer_function_legend_ptr->set_color_map(ctx, transfer_function);
		}
	}

	volume_frame_buffer.ensure(ctx);
}

void slice_renderer::draw(cgv::render::context& ctx) 
{
	// default render style for the bounding box
	static const cgv::render::box_wire_render_style box_rs;

	
	volume_frame_buffer.enable(ctx);
	glClear(GL_COLOR_BUFFER_BIT);

	// render the wireframe bounding box if enabled
	if(show_box)
		box_rd.render(ctx, cgv::render::ref_box_wire_renderer(ctx), box_rs);


	
	// render the volume
	auto& vr = cgv::render::ref_volume_renderer(ctx);
	vr.set_render_style(vstyle);
	vr.set_volume_texture(&volume_tex); // set volume texture as 3D scalar input data
	vr.set_transfer_function_texture(&transfer_function.ref_texture()); // get the texture from the transfer function color map to transform scalar volume values into RGBA colors
	// set the volume bounding box and enable transform to automatically place and size the volume to the defined bounds
	vr.set_bounding_box(volume_bounding_box);
	vr.transform_to_bounding_box(true);

	vr.render(ctx, 0, 0);
	
	volume_frame_buffer.disable(ctx);

	volume_frame_buffer.enable_attachment(ctx, "COLOR", 0);

	// Get the shader program
	auto& shader_program = ctx.ref_default_shader_program(true);
	// Enable it
	shader_program.enable(ctx);
	shader_program.set_uniform(ctx, "gamma", 1.0f);
	
	// Pass in the current context and the default shader program of the context (the function automatically enables and disables the shader program)
	cgv::render::gl::cover_screen(ctx, &shader_program);

	// Disable the shader program
	shader_program.disable(ctx);

	volume_frame_buffer.disable_attachment(ctx, "COLOR");

	
}

void slice_renderer::after_finish(cgv::render::context& context)
{
	application_plugin::after_finish(context);

	// Check if we want to generate a screenshot, if so check if we are in the correct render pass
	if (store_next_screenshot && context.get_render_pass() == cgv::render::RP_MAIN)
	{
		// Call our screenshot function
		save_buffer_to_file(context);
	}
}

void slice_renderer::create_gui() 
{
	add_decorator("Volume Viewer", "heading", "level=2");
	
	add_member_control(this, "Show Box", show_box, "check");

	add_decorator("Generation Parameters", "heading", "level=3");
	add_member_control(this, "Randomize Zoom", randomize_zoom, "check");
	add_member_control(this, "Randomize Offset", randomize_offset, "check");
	add_member_control(this, "Sample Count", sample_count, "value_slider", "min=1;max=1000;step=1;");
	add_member_control(this, "X Resolution", sample_width, "value_slider", "min=128;max=4096;step=32;");
	add_member_control(this, "Y Resolution", sample_height, "value_slider", "min=128;max=4096;step=32;");
	connect_copy(add_button("Apply Resolution")->click, cgv::signal::rebind(this, &slice_renderer::resize_render_target));
	connect_copy(add_button("Generate Samples")->click, cgv::signal::rebind(this, &slice_renderer::generate_samples));
	add_decorator("Data Exports", "heading", "level=3");
	connect_copy(add_button("Export Transfer Function")->click, cgv::signal::rebind(this, &slice_renderer::export_transfer_function));
	connect_copy(add_button("Export Volume")->click, cgv::signal::rebind(this, &slice_renderer::export_volume_data));
	
	
	if(begin_tree_node("Volume Rendering", vstyle, true)) {
		align("\a");
		add_gui("vstyle", vstyle);
		align("\b");
		end_tree_node(vstyle);
	}

	if(begin_tree_node("Bounding Box", volume_bounding_box, false)) {
		align("/a");
		vec3& a = volume_bounding_box.ref_min_pnt();
		vec3& b = volume_bounding_box.ref_max_pnt();

		add_member_control(this, "Min X", a.x(), "value_slider", "min=-1;max=1;step=0.05;");
		add_member_control(this, "Y", a.y(), "value_slider", "min=-1;max=1;step=0.05;");
		add_member_control(this, "Z", a.z(), "value_slider", "min=-1;max=1;step=0.05;");

		add_member_control(this, "Max X", b.x(), "value_slider", "min=-1;max=1;step=0.05;");
		add_member_control(this, "Y", b.y(), "value_slider", "min=-1;max=1;step=0.05;");
		add_member_control(this, "Z", b.z(), "value_slider", "min=-1;max=1;step=0.05;");
		align("/b");
		end_tree_node(volume_bounding_box);
	}
	add_decorator("Scaling", "heading", "level=3");
	connect_copy(add_button("Fit to Resolution")->click, cgv::signal::rebind(this, &slice_renderer::fit_to_resolution));
	connect_copy(add_button("Fit to Spacing")->click, cgv::signal::rebind(this, &slice_renderer::fit_to_spacing));
	connect_copy(add_button("Fit to Both")->click, cgv::signal::rebind(this, &slice_renderer::fit_to_resolution_and_spacing));

	add_decorator("Transfer Function", "heading", "level=3");
	add_member_control(this, "Preset", transfer_function_preset_idx, "dropdown", "enums='#1 (White),#2,#3 (Aneurysm),#4 (Head)'");

	inline_object_gui(transfer_function_editor_ptr);
	
	inline_object_gui(transfer_function_legend_ptr);
}

void slice_renderer::handle_transfer_function_change() {

	if(auto ctx_ptr = get_context()) {
		auto& ctx = *ctx_ptr;
		if(transfer_function_editor_ptr) {
			transfer_function.generate_texture(ctx);
			if(transfer_function_legend_ptr)
				transfer_function_legend_ptr->set_color_map(ctx, transfer_function);
		}
	}
}

void slice_renderer::update_bounding_box() {

	box_rd.clear();
	box_rd.add(volume_bounding_box.get_center(), volume_bounding_box.get_extent());

	vec3& a = volume_bounding_box.ref_min_pnt();
	vec3& b = volume_bounding_box.ref_max_pnt();

	update_member(&a.x());
	update_member(&a.y());
	update_member(&a.z());
	update_member(&b.x());
	update_member(&b.y());
	update_member(&b.z());
	
	post_redraw();
}

void slice_renderer::load_transfer_function_preset() {

	unsigned idx = static_cast<unsigned>(transfer_function_preset_idx);
	idx = std::min(idx, 3u);

	transfer_function.clear();

	switch(idx) {
	case 0:
		// plain white with linear opacity ramp
		transfer_function.add_color_point(0.0f, rgb(1.0f));
		transfer_function.add_opacity_point(0.0f, 0.0f);
		transfer_function.add_opacity_point(1.0f, 1.0f);
		break;
	case 1:
		// blue -> red -> yellow, optimized for example volume
		transfer_function.add_color_point(0.0f, rgb(0.0f, 0.0f, 1.0f));
		transfer_function.add_color_point(0.5f, rgb(1.0f, 0.0f, 0.0f));
		transfer_function.add_color_point(1.0f, rgb(1.0f, 1.0f, 0.0f));

		transfer_function.add_opacity_point(0.05f, 0.0f);
		transfer_function.add_opacity_point(0.1f, 0.1f);
		transfer_function.add_opacity_point(0.3f, 0.1f);
		transfer_function.add_opacity_point(0.35f, 0.0f);
		transfer_function.add_opacity_point(0.35f, 0.0f);
		transfer_function.add_opacity_point(0.45f, 0.0f);
		transfer_function.add_opacity_point(0.5f, 0.15f);
		transfer_function.add_opacity_point(0.55f, 0.15f);
		transfer_function.add_opacity_point(0.6f, 0.0f);
		transfer_function.add_opacity_point(0.8f, 0.0f);
		transfer_function.add_opacity_point(0.95f, 0.5f);
		break;
	case 2:
		// optimized for aneurysm.vox
		transfer_function.add_color_point(0.0f, rgb(1.0f, 1.0f, 1.0f));
		transfer_function.add_color_point(0.25f, rgb(0.95f, 1.0f, 0.8f));
		transfer_function.add_color_point(1.0f, rgb(1.0f, 0.4f, 0.333f));

		transfer_function.add_opacity_point(0.1f, 0.0f);
		transfer_function.add_opacity_point(1.0f, 1.0f);
		break;
	case 3:
		// optimized for head256.vox
		transfer_function.add_color_point(0.332f, rgb(0.5f, 0.8f, 0.85f));
		transfer_function.add_color_point(0.349f, rgb(0.85f, 0.5f, 0.85f));
		transfer_function.add_color_point(0.370f, rgb(0.9f, 0.85f, 0.8f));
		transfer_function.add_color_point(0.452f, rgb(0.9f, 0.85f, 0.8f));
		transfer_function.add_color_point(0.715f, rgb(0.9f, 0.85f, 0.8f));
		transfer_function.add_color_point(1.0f, rgb(1.0f, 0.0f, 0.0f));

		transfer_function.add_opacity_point(0.208f, 0.0f);
		transfer_function.add_opacity_point(0.22f, 0.17f);
		transfer_function.add_opacity_point(0.315f, 0.17f);
		transfer_function.add_opacity_point(0.326f, 0.0f);
		transfer_function.add_opacity_point(0.345f, 0.0f);
		transfer_function.add_opacity_point(0.348f, 0.23f);
		transfer_function.add_opacity_point(0.35f, 0.0f);
		transfer_function.add_opacity_point(0.374f, 0.0f);
		transfer_function.add_opacity_point(0.539f, 0.31f);
		transfer_function.add_opacity_point(0.633f, 0.31f);
		transfer_function.add_opacity_point(0.716f, 0.0f);
		transfer_function.add_opacity_point(0.8f, 1.0f);
		break;
	default: break;
	}
	
	if(auto ctx_ptr = get_context()) {
		// generate the texture containing the interpolated color map values
		transfer_function.generate_texture(*ctx_ptr);

		if(transfer_function_editor_ptr)
			transfer_function_editor_ptr->set_color_map(&transfer_function);
		if(transfer_function_legend_ptr)
			transfer_function_legend_ptr->set_color_map(*ctx_ptr, transfer_function);
	}
}

void slice_renderer::create_volume(cgv::render::context& ctx) {
	// destruct previous texture
	volume_tex.destruct(ctx);

	// calculate voxel size
	float voxel_size = 1.0f / vres.x();

	// generate volume data
	vol_data.clear();
	vol_data.resize(vres[0] * vres[1] * vres[2], 0.0f);

	std::mt19937 rng(42);
	std::uniform_real_distribution<float> distr(0.0f, 1.0f);

	const vec3& a = volume_bounding_box.ref_min_pnt();
	const vec3& b = volume_bounding_box.ref_max_pnt();

	// generate a single large sphere in the center of the volume
	splat_sphere(vol_data, voxel_size, 0.5f*(a + b), 0.5f, 0.75f);

	// add and subtract volumes of an increasing amount of randomly placed spheres of decreasing size
	splat_spheres(vol_data, voxel_size, rng, 5, 0.2f, 0.5f);
	splat_spheres(vol_data, voxel_size, rng, 5, 0.2f, -0.5f);

	splat_spheres(vol_data, voxel_size, rng, 50, 0.1f, 0.25f);
	splat_spheres(vol_data, voxel_size, rng, 50, 0.1f, -0.25f);

	splat_spheres(vol_data, voxel_size, rng, 100, 0.05f, 0.1f);
	splat_spheres(vol_data, voxel_size, rng, 100, 0.05f, -0.1f);

	splat_spheres(vol_data, voxel_size, rng, 200, 0.025f, 0.1f);
	splat_spheres(vol_data, voxel_size, rng, 200, 0.025f, -0.1f);

	// make sure the volume values are in the range [0,1]
	for(size_t i = 0; i < vol_data.size(); ++i)
		vol_data[i] = cgv::math::clamp(vol_data[i], 0.0f, 1.0f);

	// transfer volume data into volume texture
	// and compute mipmaps
	cgv::data::data_format vol_df(vres[0], vres[1], vres[2], cgv::type::info::TypeId::TI_FLT32, cgv::data::ComponentFormat::CF_R);
	cgv::data::const_data_view vol_dv(&vol_df, vol_data.data());
	volume_tex.create(ctx, vol_dv, 0);

	// set the volume bounding box to later scale the rendering accordingly
	volume_bounding_box.ref_min_pnt() = volume_bounding_box.ref_min_pnt();
	volume_bounding_box.ref_max_pnt() = volume_bounding_box.ref_max_pnt();

	// calculate a histogram
	create_histogram();
}

// splats n spheres of given radius into the volume, by adding the contribution to the covered voxel cells
void slice_renderer::splat_spheres(std::vector<float>& vol_data, float voxel_size, std::mt19937& rng, size_t n, float radius, float contribution) {
	std::uniform_real_distribution<float> distr(0.0f, 1.0f);

	const vec3& a = volume_bounding_box.ref_min_pnt();
	const vec3& b = volume_bounding_box.ref_max_pnt();

	for(size_t i = 0; i < n; ++i) {
		vec3 pos;
		pos.x() = cgv::math::lerp(a.x(), b.x(), distr(rng));
		pos.y() = cgv::math::lerp(a.y(), b.y(), distr(rng));
		pos.z() = cgv::math::lerp(a.z(), b.z(), distr(rng));
		splat_sphere(vol_data, voxel_size, pos, radius, contribution);
	}
}

// splats a single sphere of given radius into the volume by adding the contribution value to the voxel cells
void slice_renderer::splat_sphere(std::vector<float>& vol_data, float voxel_size, const vec3& pos, float radius, float contribution) {

	// compute the spheres bounding box
	box3 box(pos - radius, pos + radius);
	box.ref_max_pnt() -= 0.005f * voxel_size;

	// get voxel indices of bounding box minimum and maximum
	ivec3 sidx((box.get_min_pnt() - volume_bounding_box.ref_min_pnt()) / voxel_size);
	ivec3 eidx((box.get_max_pnt() - volume_bounding_box.ref_min_pnt()) / voxel_size);

	const ivec3 res = static_cast<ivec3>(vres);

	// make sure to stay inside the volume
	sidx = cgv::math::clamp(sidx, ivec3(0), res - 1);
	eidx = cgv::math::clamp(eidx, ivec3(0), res - 1);

	// for each covered voxel...
	for(int z = sidx.z(); z <= eidx.z(); ++z) {
		for(int y = sidx.y(); y <= eidx.y(); ++y) {
			for(int x = sidx.x(); x <= eidx.x(); ++x) {
				// ...get its center location in world space
				vec3 voxel_pos(
					static_cast<float>(x),
					static_cast<float>(y),
					static_cast<float>(z)
				);
				voxel_pos *= voxel_size;
				voxel_pos += volume_bounding_box.ref_min_pnt() + 0.5f*voxel_size;

				// calculate the distance to the sphere center
				float dist = length(voxel_pos - pos);
				// add contribution to voxel if its center is inside the sphere
				if(dist < radius) {
					// modulate contribution by distance to sphere center
					float dist_factor = 1.0f - (dist / radius);
					dist_factor = sqrt(dist_factor);
					vol_data[x + vres.x()*y + vres.x()*vres.y()*z] += contribution * dist_factor;
				}
			}
		}
	}
}

void slice_renderer::load_volume_from_file(const std::string& file_name) {

	std::string header_content;
	char* vox_content;

	std::string hd_file_name = "";
	std::string vox_file_name = "";

	std::string extension = cgv::utils::file::get_extension(file_name);
	if(cgv::utils::to_upper(extension) == "HD") {
		hd_file_name = file_name;
		vox_file_name = file_name.substr(0, file_name.length() - 2) + "vox";
	} else if(cgv::utils::to_upper(extension) == "VOX") {
		hd_file_name = file_name.substr(0, file_name.length() - 3) + "hd";
		vox_file_name = file_name;
	}

	if(!cgv::utils::file::exists(hd_file_name) || !cgv::utils::file::exists(vox_file_name))
		return;

	std::cout << "Loading volume from: ";
	std::cout << vox_file_name << std::endl;

	if(!cgv::utils::file::read(hd_file_name, header_content, true)) {
		std::cout << "Error: failed to read header file." << std::endl;
		return;
	}

	ivec3 resolution(-1);
	vec3 spacing(1.0f);

	std::vector<cgv::utils::line> lines;
	cgv::utils::split_to_lines(header_content, lines);

	for(const auto& line : lines) {
		std::vector<cgv::utils::token> tokens;
		cgv::utils::split_to_tokens(line, tokens, "x", true, "", "", " x,");

		if(tokens.size() == 0)
			continue;

		std::string identifier = to_string(tokens[0]);
		if(identifier.length() == 0)
			continue;

		if(identifier.back() == ':')
			identifier = identifier.substr(0, identifier.length() - 1);

		if(identifier == "Size" || identifier == "Dimension") {
			int idx = 0;
			for(size_t i = 1; i < tokens.size(); ++i) {
				std::string str = to_string(tokens[i]);

				char* p_end;
				const long num = std::strtol(str.c_str(), &p_end, 10);
				if(str.c_str() != p_end) {
					resolution[idx] = static_cast<int>(num);
					++idx;
					if(idx > 2)
						break;
				}
			}
		} else if(identifier == "Spacing") {
			int idx = 0;
			for(size_t i = 1; i < tokens.size(); ++i) {
				std::string str = to_string(tokens[i]);

				char* p_end;
				const float num = std::strtof(str.c_str(), &p_end);
				if(str.c_str() != p_end) {
					spacing[idx] = num;
					++idx;
					if(idx > 2)
						break;
				}
			}
		} else {
			std::cout << "Warning: unknown identifier <" + identifier + ">" << std::endl;
		}
	}

	std::cout << "[resolution] = " << resolution << std::endl;
	std::cout << "[spacing]    = " << spacing << std::endl;

	if(cgv::math::min_value(resolution) < 0) {
		std::cout << "Error: could not read valid resolution." << std::endl;
		return;
	}

	if(cgv::math::min_value(spacing) < 0.0f) {
		std::cout << "Error: could not read valid spacing." << std::endl;
		return;
	}

	auto ctx_ptr = get_context();
	if(ctx_ptr) {
		auto& ctx = *ctx_ptr;

		vres = resolution;
		vspacing = spacing;

		size_t num_voxels = resolution.x() * resolution.y() * resolution.z();

		vol_data.clear();
		vol_data.resize(num_voxels, 0.0f);

		std::vector<unsigned char> raw_vol_data(num_voxels, 0u);

		FILE* fp = fopen(vox_file_name.c_str(), "rb");
		if(fp) {
			std::size_t nr = fread(raw_vol_data.data(), 1, num_voxels, fp);
			if(nr != num_voxels) {
				std::cout << "Error: could not read the expected number " << num_voxels << " of voxels but only " << nr << "." << std::endl;
				fclose(fp);
			}
		} else {
			std::cout << "Error: failed to read voxel file." << std::endl;
		}
		fclose(fp);

		for(size_t i = 0; i < num_voxels; ++i)
			vol_data[i] = static_cast<float>(raw_vol_data[i] / 255.0f);

		if(volume_tex.is_created())
			volume_tex.destruct(ctx);

		cgv::data::data_format vol_df(resolution[0], resolution[1], resolution[2], cgv::type::info::TypeId::TI_FLT32, cgv::data::ComponentFormat::CF_R);
		cgv::data::const_data_view vol_dv(&vol_df, vol_data.data());
		volume_tex.create(ctx, vol_dv, 0);

		fit_to_resolution();
	}

	create_histogram();
}

void slice_renderer::fit_to_resolution() {

	unsigned max_resolution = max_value(vres);
	vec3 scaling = static_cast<vec3>(vres) / static_cast<float>(max_resolution);

	volume_bounding_box.ref_min_pnt() = vec3(-0.5f*scaling);
	volume_bounding_box.ref_max_pnt() = vec3(+0.5f*scaling);

	update_bounding_box();
}

void slice_renderer::fit_to_spacing() {

	volume_bounding_box.ref_min_pnt() = vec3(-0.5f*vspacing);
	volume_bounding_box.ref_max_pnt() = vec3(+0.5f*vspacing);

	update_bounding_box();
}

void slice_renderer::fit_to_resolution_and_spacing() {

	unsigned max_resolution = max_value(vres);
	vec3 scaling = static_cast<vec3>(vres) / static_cast<float>(max_resolution);
	scaling *= vspacing;

	volume_bounding_box.ref_min_pnt() = vec3(-0.5f*scaling);
	volume_bounding_box.ref_max_pnt() = vec3(+0.5f*scaling);

	update_bounding_box();
}

void slice_renderer::create_histogram() {
	std::vector<unsigned> histogram(128, 0u);

	for(size_t i = 0; i < vol_data.size(); ++i) {
		size_t bucket = static_cast<size_t>(vol_data[i] * 128.0f);
		size_t min = 0;
		size_t max = 127;
		bucket = cgv::math::clamp(bucket, min, max);
		++histogram[bucket];
	}

	if(transfer_function_editor_ptr)
		transfer_function_editor_ptr->set_histogram_data(histogram);
}

// Uniformly sample a point on the surface of a sphere
cgv::render::vec3 slice_renderer::sample_sphere()
{
	const float theta = 2.0f * PI * dist(rng);
	const float phi = acos(1.0f - 2.0f * dist(rng));

	return vec3(sin(phi) * cos(theta), sin(phi) * sin(theta), cos(phi));
}

void slice_renderer::center_and_zoom(float zoom = 1.0f) const
{
	
	if (view_ptr)
	{

		// Ensure the focus point in the center
		view_ptr->set_focus(vec3(0.0f, 0.0f, 0.0f));

		// Set the FOV to 90 degrees
		view_ptr->set_y_view_angle(45.0);

		// Get the angle by subtracting 90 of the FOV and convert it to radians
		const float angle = (90.0 - view_ptr->get_y_view_angle() * 0.5) * PI / 180.0;

		
		// Get the radius of the bounding box
		const float radius = volume_bounding_box.get_extent().length();

		// In our viewport we want to show the entire bounding sphere, not only the top point
		// To calculate the y_extent_at_focus we use the radius as the height in a right-angled triangle with the FOV as the angle
		// The y_extent_at_focus is the distance from the focus point to the top of the viewport
		// So our setup is the following:
		/*
		|\ --- y_extent_at_focus
		| \   
		|  \ radius (of the bounding box)
	  a |  /\
		| /r \ c
		|/    \
  90deg |------| <- angle = y_view_angle 
			b
		*/
		// We can calculate the height of the triangle with the following formula:
		// we need the angle between a and c, which is 90 - angle (in degrees)
		// radius = sin(0.5 PI - angle) * y_extent_at_focus
		// y_extent_at_focus = radius / sin(0.5 PI - angle)


		// Also get the aspect ratio between width and height
		// If it is wider than it is high, we need to adjust the y_extent_at_focus
		const float aspect_ratio = static_cast<float>(sample_width) / static_cast<float>(sample_height);

		const float extent_factor = (radius / sin(angle))*zoom;
		
		if (aspect_ratio >= 1.0f)
			view_ptr->set_y_extent_at_focus(extent_factor);
		// This fix is not mathematically correct, but it is relatively close, so sufficient for the rare cases 
		else
			view_ptr->set_y_extent_at_focus((extent_factor + extent_factor / aspect_ratio)/2.0f);
		
	}
}

void slice_renderer::resize_render_target() const
{
	if(const auto ctx_ptr = get_context())
	{
		// To resize our render target, we resize our context's render target.
		ctx_ptr->resize(sample_width, sample_height);

		center_and_zoom();
	}

}

void slice_renderer::generate_samples()
{

	
	std::cout << "Generating " << sample_count << " samples ..." << std::endl;

	// Get the context
	const auto ctx_ptr = get_context();

	
	auto old_gamma = ctx_ptr->get_gamma();
		
	ctx_ptr->set_gamma(1.0);


	// Exit if we don't have a context
	if (!ctx_ptr)
	{
		std::cout << "No context found!" << std::endl;
		return;
	}

	ctx_ptr->force_redraw();

	// Delete the old output folder
	if (std::filesystem::exists("./out/images"))
	{
		std::filesystem::remove_all("./out/images");
	}
	
	// Create the folder again
	std::filesystem::create_directory("./out/images");
		

	// Create the JSON data structure which stores information about the samples

	// After taking a look at all possible parameters
	//	'camera_angle_x', 'camera_angle_y', 'fl_x', 'fl_y', 'k1', 'k2', 'k3', 'k4', 'p1', 'p2', 'is_fisheye', 'cx', 'cy', 'w', 'h', 'aabb_scale'
	// most of them are actually optional and not needed for our use case, additionally instead of 'camera_angle_x' and 'camera_angle_y' one could also use either 'fl_x' and 'fl_y' or "x_fov" and "y_fov", as only one of them is read
	// So for easier use we use x_fov and y_fov in degrees, as we have them available in the view
	// cx and cy can also be left out, as they are set to the center of the image by default and in the generated samples
	// Additionally the parameter scale exists, as their default datasets are oversized, they have a scale of 0.33 for them
	// We are already in a unit cube, so we can set it to 1

	// Calculate the X fov from the Y fov
	const float aspect_ratio = static_cast<float>(sample_width) / static_cast<float>(sample_height);
	const float x_fov = 2.0f * std::atan(std::tan(view_ptr->get_y_view_angle() * 0.5f * PI / 180.0f) * aspect_ratio) * 180.0f / PI;

	json frames_array = json::array();

	json sample_info = {
		{"y_fov", view_ptr->get_y_view_angle()},
		{"x_fov", x_fov},
		{"w", sample_width},
		{"h", sample_height},
		{"aabb_scale", 2.0f},
		{"frames", frames_array}
	};
	
	// Generate the samples
	for (size_t i = 0; i < sample_count; ++i)
	{
		// Generate a random rotation
		const auto view_rotation = sample_sphere();
		const auto view_rotation_up = sample_sphere();

		// Set the rotation of the view
		view_ptr->set_view_dir(view_rotation);
		//view_ptr->set_view_up_dir(view_rotation_up);
		// Set the up direction to the y axis
		vec3 up_dir = { 0.0f, 1.0f, 0.0f };
		view_ptr->set_view_up_dir(up_dir);

		// Cause a redraw
		ctx_ptr->force_redraw();

		// Center and zoom the view

		float zoom = 1.0f;
		
		if (randomize_zoom)
		{
			// Sample a normal distribution with mean 1.0 and small standard deviation
			zoom = std::clamp(std::normal_distribution<float>(1.0f, 0.3f)(rng), 0.1f, 2.0f);
		}
		
		center_and_zoom(zoom);

		// Add very small left and right pan to the view
		if (randomize_offset)
		{
			view_ptr->pan(dist(rng) - 0.5, dist(rng) - 0.5);
		}

		// Save the image to the output directory
		const std::string filename = dump_image_to_path("./out/images/generation.png");

		// Remove the out directory from the path
		const std::string file_path = filename.substr(5);

		// Store the information about the sample in the JSON data structure
		// The data structure normally is file_path, sharpness and transform_matrix
		// We can leave sharpness out as we take every image
		// The transform matrix is a 4x4 matrix which represents the camera extrinsics
		// They are in the following format:
		// [+X0 +Y0 +Z0 X]
		// [+X1 +Y1 +Z1 Y]
		// [+X2 +Y2 +Z2 Z]
		// [0.0 0.0 0.0 1]
		// (See https://docs.nerf.studio/en/latest/quickstart/data_conventions.html for details)

		// Get the camera position
		const auto camera_position = view_ptr->get_eye();
		auto forward = cgv::math::normalize(view_ptr->get_focus() - camera_position);
		const auto right = cgv::math::normalize(cgv::math::cross(forward, view_ptr->get_view_up_dir()));
		const auto upward = cgv::math::normalize(cgv::math::cross(right, forward));
		
		// Construct the new json object
		frames_array += {
			{"file_path", file_path},
			{
				"transform_matrix",
				{
					{right(0), upward(0), -forward(0), camera_position(0)},
					{-right(2), -upward(2), forward(2), -camera_position(2)},
					{right(1), upward(1), -forward(1), camera_position(1)},
					{0, 0, 0, 1}
				}
			}
		};
	}

	ctx_ptr->set_gamma(old_gamma);

	// Make sure frames_array is in the json data structure
	sample_info["frames"] = frames_array;

	// Write the JSON data structure to a file transforms.json
	// Write it to console that the file was written
	std::cout << "Writing sample info to file ..." << std::endl;
	std::ofstream file("./out/transforms.json");
	file << sample_info.dump(2);
}

void slice_renderer::export_transfer_function()
{
	if(auto ctx_ptr = get_context())
	{
		auto texture_reference = transfer_function.ref_texture();

		// Get the source texture from opengl using glGetTexImage
		// The texture is a 1D texture with 4 components (RGBA)

		const int width = texture_reference.get_width();
		
		// Create a vector to store the texture data
		GLubyte* texture_data = new GLubyte[width * 4];

		// Activate the texture unit
		texture_reference.enable(*ctx_ptr);

		// Get the texture data
		glGetTexImage(GL_TEXTURE_1D, 0, GL_RGBA, GL_UNSIGNED_BYTE, texture_data);

		// Get last error
		const auto error = glGetError();

		if (error != GL_NO_ERROR)
		{
			std::cout << "Error while getting texture data: " << error << std::endl;
		}

		// Deactivate the texture unit
		texture_reference.disable(*ctx_ptr);

		// Save the texture data to a file

		// Create a buffer to store the data
		std::vector<uint8_t> data_buffer;

		// Use fpng to write the data into the buffer
		fpng::fpng_encode_image_to_memory( texture_data, width, 1, 4, data_buffer);

		// Write the buffer to the file using a fstream
		std::ofstream file("./out/transfer_function.png", std::ios::out | std::ios::binary);

		// Write the data to the file
		file.write(reinterpret_cast<char*>(data_buffer.data()), data_buffer.size());

		// Close the file
		file.close();

		std::cout << "Wrote transfer function to file: " << "./out/transfer_function.png" << std::endl;
	} 
}

void slice_renderer::export_volume_data()
{
	if(auto ctx_ptr = get_context())
	{
		// Create a vector<char> representing the volume by converting the current volume to a 8 bit unsigned integer
		std::vector<uint8_t> volume_data(vres[0] * vres[1] * vres[2]);

		// Iterate the old floating point based volume and store the values in the new vector
		for (size_t i = 0; i < volume_data.size(); ++i)
		{
			volume_data[i] = static_cast<uint8_t>(255.0f * vol_data[i]);
		}

		// Write this vector to a file
		std::ofstream file("./out/volume_data.vox", std::ios::binary);

		file.write(reinterpret_cast<char*>(volume_data.data()), volume_data.size());

		file.close();

		// We additionally create a header for the file which contains the resolution of the volume
		// For that create a .hd file with the same name where we just write in the resolution as text
		std::ofstream header_file("./out/volume_data.hd");

		header_file << "Size " << vres[0] << "x" << vres[1] << "x" << vres[2];

		header_file.close();

		std::cout << "Wrote volume data to file: " << "./out/volume_data.vox" << std::endl;
	} 
}

void slice_renderer::save_buffer_to_file(cgv::render::context& ctx)
{
	// Check if we are in the correct render pass and the flag is enabled
	if (ctx.get_render_pass() != cgv::render::RP_MAIN || !store_next_screenshot)
		return;

	// Disable the flag
	store_next_screenshot = false;

	// Pick the best supported format
	std::string extension = "bmp";
	const std::string available_extensions = cgv::media::image::image_writer::get_supported_extensions();

	// Extract the extension from the file name
	const std::string::size_type pos = screenshot_filename.find_last_of('.');
	if (pos != std::string::npos)
		extension = screenshot_filename.substr(pos + 1);

	// If the extension is not supported, use the first supported extension
	if (available_extensions.find(extension) == std::string::npos)
	{
		// Prefer png if available
		if (available_extensions.find("png") != std::string::npos)
			extension = "png";

		// Next take jpg
		else if (available_extensions.find("jpg") != std::string::npos)
			extension = "jpg";

		// If all else fails take tif
		else if (available_extensions.find("tif") != std::string::npos)
			extension = "tif";
	}
	
	// Check if the file already exists on the disk, if so append a number to the file name
	std::string filename = screenshot_filename;
	if (cgv::utils::file::exists(filename))
	{
		int i = 0;
		do
		{
			filename = screenshot_filename.substr(0, pos) + "_" + std::to_string(i) + "." + extension;
			++i;
		} while (cgv::utils::file::exists(filename));
	}
	// Time the screenshot generation
	std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
	

	// Now pass it into our method to generate the image
	ctx.write_frame_buffer_to_image(filename);

	// Get the time it took to generate the image
	std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
	// Log how long it took to generate the screenshot with the specific name
	std::cout << "Screenshot " << filename << " generated in " << std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count() << "ms" << std::endl;
}

const std::string slice_renderer::dump_image_to_path(const std::string& file_path)
{
	if(auto ctx_ptr = get_context())
	{
		// Only png is supported with this implementation
		std::string extension = "png";

		// Extract the extension from the file name
		const std::string::size_type pos = file_path.find_last_of('.');
		
		

		// Check if the file already exists on the disk, if so append a number to the file name
		std::string filename = file_path;

		// Ensure filename has png as extension
		filename = filename.substr(0, pos) + "." + extension;
		
		// Check if the file already exists on the disk, if so append a number to the file name
		if (cgv::utils::file::exists(filename))
		{
			int i = 0;
			do
			{
				filename = file_path.substr(0, pos) + "_" + std::to_string(i) + "." + extension;
				++i;
			} while (cgv::utils::file::exists(filename));
		}
		
		// Time the screenshot generation
		std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

		// We have the image we want in a buffer, and in that buffer in a texture, so enable that texture
		volume_frame_buffer.enable_attachment(*ctx_ptr, "COLOR", 0);

		// Next create a GLubyte array to store the data
		GLubyte* data = new GLubyte[volume_frame_buffer.get_size().x() * volume_frame_buffer.get_size().y() * 4];

		// Read the data from the texture into the array
		glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);

		// Disable the attachment
		volume_frame_buffer.disable_attachment(*ctx_ptr, "COLOR");

		// Loop through the data and flip the image vertically
		for (int i = 0; i < volume_frame_buffer.get_size().y() / 2; ++i)
		{
			for (int j = 0; j < volume_frame_buffer.get_size().x(); ++j)
			{
				for (int k = 0; k < 4; ++k)
				{
					std::swap(data[i * volume_frame_buffer.get_size().x() * 4 + j * 4 + k], data[(volume_frame_buffer.get_size().y() - i - 1) * volume_frame_buffer.get_size().x() * 4 + j * 4 + k]);
				}
			}
		}

		
		// Create a buffer to store the data
		std::vector<uint8_t> data_buffer;

		// Use fpng to write the data into the buffer
		fpng::fpng_encode_image_to_memory( data, volume_frame_buffer.get_size().x(), volume_frame_buffer.get_size().y(), 4, data_buffer);

		// Write the buffer to the file using a fstream
		std::ofstream file(filename, std::ios::out | std::ios::binary);

		// Write the data to the file
		file.write(reinterpret_cast<char*>(data_buffer.data()), data_buffer.size());

		// Close the file
		file.close();
		
		// Get the time it took to generate the image
		std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
		// Log how long it took to generate the screenshot with the specific name
		std::cout << "Screenshot " << filename << " generated in " << std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count() << "ms" << std::endl;
		return filename;
	} else
	{
		std::cerr << "Failed to get context" << std::endl;
		return "";
	}
}

#include <cgv/base/register.h>

cgv::base::object_registration<slice_renderer> slice_renderer_reg("slice_renderer");