///|/ Slice it All
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "GLGizmoBrimEars.hpp"

#include "slic3r/GUI/Gizmos/GLGizmosCommon.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/ImGuiPureWrap.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/Utils/UndoRedo.hpp"

#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <algorithm>
#include <GL/glew.h>

namespace Slic3r {
namespace GUI {

GLGizmoBrimEars::GLGizmoBrimEars(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id)
    : GLGizmoBase(parent, icon_filename, sprite_id)
{
}

bool GLGizmoBrimEars::on_init()
{
    m_shortcut_key = WXK_CONTROL_B;
    return true;
}

std::string GLGizmoBrimEars::on_get_name() const
{
    return _u8L("Brim ears");
}

void GLGizmoBrimEars::on_render()
{
}

CommonGizmosDataID GLGizmoBrimEars::on_get_requirements() const
{
    return CommonGizmosDataID(0);
}

bool GLGizmoBrimEars::on_is_activable() const
{
    const Selection& selection = m_parent.get_selection();
    return selection.is_single_full_instance() && !selection.is_wipe_tower();
}

void GLGizmoBrimEars::add_ear()
{
    const Selection& selection = m_parent.get_selection();
    if (!selection.is_single_full_instance())
        return;

    const int obj_idx = selection.get_object_idx();
    const int inst_idx = selection.get_instance_idx();
    if (obj_idx < 0 || inst_idx < 0)
        return;

    Plater* plater = wxGetApp().plater();
    ModelObject* model_object = plater->model().objects[obj_idx];
    const GLVolume* gl_volume = selection.get_first_volume();
    if (model_object == nullptr || gl_volume == nullptr)
        return;

    Plater::TakeSnapshot snapshot(plater, _L("Add brim ear"), UndoRedo::SnapshotType::GizmoAction);

    const double radius = 0.5 * double(std::max(m_diameter, 0.5f));
    const double height = std::max(0.05, double(m_thickness));
    ModelVolume* volume = model_object->add_volume(TriangleMesh(its_make_cylinder(radius, height)), ModelVolumeType::MODEL_PART);
    volume->name = into_u8(_L("Brim ear"));
    volume->config.set_key_value("extruder", new ConfigOptionInt(0));

    const BoundingBoxf3 instance_bb = model_object->instance_bounding_box(inst_idx);
    const Vec3d corners[4] = {
        Vec3d(instance_bb.min.x(), instance_bb.min.y(), instance_bb.min.z()),
        Vec3d(instance_bb.max.x(), instance_bb.min.y(), instance_bb.min.z()),
        Vec3d(instance_bb.max.x(), instance_bb.max.y(), instance_bb.min.z()),
        Vec3d(instance_bb.min.x(), instance_bb.max.y(), instance_bb.min.z())
    };
    const Vec3d corner = corners[m_next_corner++ & 3];
    const Transform3d align = gl_volume->get_instance_transformation().get_matrix_no_offset().inverse();
    volume->set_transformation(align);
    volume->set_offset(align * (corner - gl_volume->get_instance_offset()));

    wxGetApp().obj_list()->update_after_undo_redo();
}

void GLGizmoBrimEars::on_render_input_window(float x, float y, float bottom_limit)
{
    ImGuiPureWrap::begin(get_name(), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
    GLGizmoBase::place_input_window(get_name(), x, y, bottom_limit);

    ImGui::TextWrapped("%s", _u8L("Adds a thin disc on the selected object. Each click places the next ear on another corner.").c_str());
    ImGui::Separator();

    ImGuiPureWrap::text(_u8L("Diameter"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.f);
    ImGui::SliderFloat("##brim_ear_diameter", &m_diameter, 2.f, 40.f, "%.1f mm");

    ImGuiPureWrap::text(_u8L("Thickness"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.f);
    ImGui::SliderFloat("##brim_ear_thickness", &m_thickness, 0.1f, 2.f, "%.2f mm");

    if (ImGui::Button(_u8L("Add ear").c_str()))
        add_ear();

    ImGuiPureWrap::end();
}

} // namespace GUI
} // namespace Slic3r
