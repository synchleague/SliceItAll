///|/ Copyright (c) Prusa Research 2020 - 2023 Oleksandra Iushchenko @YuSanka, Tomáš Mészáros @tamasmeszaros, Lukáš Matěna @lukasmatena
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "RotoptimizeJob.hpp"

#include "libslic3r/MTUtils.hpp"
#include "libslic3r/SLA/Rotfinder.hpp"
#include "libslic3r/MinAreaBoundingBox.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/SLAPrint.hpp"

#include "slic3r/GUI/Plater.hpp"
#include "libslic3r/PresetBundle.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "libslic3r/AppConfig.hpp"

#include <slic3r/GUI/I18N.hpp>

#include <algorithm>

namespace Slic3r { namespace GUI {

void RotoptimizeJob::prepare()
{
    std::string accuracy_str =
        wxGetApp().app_config->get("sla_auto_rotate", "accuracy");

    std::string method_str =
        wxGetApp().app_config->get("sla_auto_rotate", "method_id");

    if (!accuracy_str.empty())
        m_accuracy = std::stof(accuracy_str);

    if (!method_str.empty())
        m_method_id = std::stoi(method_str);

    m_accuracy = std::max(0.f, std::min(m_accuracy, 1.f));
    m_method_id = std::max(size_t(0), std::min(get_methods_count() - 1, m_method_id));
    if (m_method_override >= 0)
        m_method_id = std::min(get_methods_count() - 1, size_t(m_method_override));

    m_default_print_cfg = wxGetApp().preset_bundle->full_config();

    m_selected_object_ids.clear();
    if (m_use_explicit_selection) {
        m_selected_object_ids.reserve(m_explicit_objects.size());
        for (size_t obj_id : m_explicit_objects)
            m_selected_object_ids.emplace_back(obj_id);
        return;
    }

    const auto &sel = m_plater->get_selection().get_content();
    m_selected_object_ids.reserve(sel.size());

    for (const auto &s : sel) {
        int obj_id;
        std::tie(obj_id, std::ignore) = s;
        m_selected_object_ids.emplace_back(obj_id);
    }
}

void RotoptimizeJob::process(Ctl &ctl)
{
    if (m_selected_object_ids.empty()) {
        ctl.update_status(100, _u8L("Orientation found."));
        return;
    }

    int prev_status = 0;
    auto statustxt = _u8L("Searching for optimal orientation");
    ctl.update_status(0, statustxt);

    auto params =
        sla::RotOptimizeParams{}
            .accuracy(m_accuracy)
            .print_config(&m_default_print_cfg)
            .statucb([this, &prev_status, &ctl, &statustxt](int s)
        {
            if (s > 0 && s < 100)
                ctl.update_status(prev_status + s / m_selected_object_ids.size(),
                                  statustxt);

            return !ctl.was_canceled();
        });


    for (ObjRot &objrot : m_selected_object_ids) {
        ModelObject *o = m_plater->model().objects[size_t(objrot.idx)];
        if (!o) continue;

        if (Methods[m_method_id].findfn)
            objrot.rot = Methods[m_method_id].findfn(*o, params);

        prev_status += 100 / m_selected_object_ids.size();

        if (ctl.was_canceled()) break;
    }

    ctl.update_status(100, ctl.was_canceled() ?
                               _u8L("Orientation search canceled.") :
                               _u8L("Orientation found."));
}

RotoptimizeJob::RotoptimizeJob() : m_plater{wxGetApp().plater()} { prepare(); }

RotoptimizeJob::RotoptimizeJob(std::vector<size_t> object_ids, std::vector<size_t> instance_ids, int method_id)
    : m_plater{wxGetApp().plater()}
{
    m_method_override = method_id;
    m_use_explicit_selection = true;
    m_explicit_objects = std::move(object_ids);
    m_limit_instance_ids = std::move(instance_ids);
    prepare();
}

void RotoptimizeJob::finalize(bool canceled, std::exception_ptr &eptr)
{
    if (canceled || eptr)
        return;

    for (const ObjRot &objrot : m_selected_object_ids) {
        ModelObject *o = m_plater->model().objects[size_t(objrot.idx)];
        if (!o) continue;

        const auto instance_limited = [this](const ModelInstance *oi) {
            return m_limit_instance_ids.empty()
                || std::find(m_limit_instance_ids.begin(), m_limit_instance_ids.end(), oi->id().id) != m_limit_instance_ids.end();
        };
        bool every_instance = true;
        for (ModelInstance *oi : o->instances) {
            if (!instance_limited(oi)) {
                every_instance = false;
                continue;
            }
            if (objrot.rot)
                oi->set_rotation({objrot.rot->x(), objrot.rot->y(), 0.});

            auto    trmatrix = oi->get_transformation().get_matrix();
            Polygon trchull  = o->convex_hull_2d(trmatrix);
            
            if (!trchull.empty()) {
                MinAreaBoundigBox rotbb(trchull, MinAreaBoundigBox::pcConvex);
                double            phi = rotbb.angle_to_X();
    
                // The box should be landscape
                if(rotbb.width() < rotbb.height()) phi += PI / 2;
    
                Vec3d rt = oi->get_rotation(); rt(Z) += phi;
    
                oi->set_rotation(rt);
            }
        }

        // Correct the z offset of the object which was corrupted be
        // the rotation. Copies on other plates keep their own rotation and height.
        o->invalidate_bounding_box();
        if (every_instance)
            o->ensure_on_bed();
        else {
            for (size_t ii = 0; ii < o->instances.size(); ++ii) {
                if (!instance_limited(o->instances[ii]))
                    continue;
                const double zmin = o->get_instance_min_z(ii);
                if (std::abs(zmin) > EPSILON)
                    o->translate_instance(ii, Vec3d(0.0, 0.0, -zmin));
            }
        }
    }

    if (!canceled)
        m_plater->update();
}

std::string RotoptimizeJob::get_method_name(size_t i)
{
    return into_u8(_(Methods[i].name));
}

std::string RotoptimizeJob::get_method_description(size_t i)
{
    return into_u8(_(Methods[i].descr));
}

}}
