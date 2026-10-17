///|/ Copyright (c) Prusa Research 2020 - 2023 Oleksandra Iushchenko @YuSanka, Tomáš Mészáros @tamasmeszaros, David Kocík @kocikdav
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef ROTOPTIMIZEJOB_HPP
#define ROTOPTIMIZEJOB_HPP

#include "Job.hpp"

#include <vector>

#include "libslic3r/SLA/Rotfinder.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/I18N.hpp"

namespace Slic3r { namespace GUI {

class Plater;

class RotoptimizeJob : public Job
{
    using FindFn = std::function<Vec2d(const ModelObject &           mo,
                                       const sla::RotOptimizeParams &params)>;

    struct FindMethod { std::string name; FindFn findfn; std::string descr; };

    static inline const FindMethod Methods[]
        = {{L("Best surface quality"),
            sla::find_best_misalignment_rotation,
            L("Optimize object rotation for best surface quality.")},
           {L("Reduced overhang slopes"),
            sla::find_least_supports_rotation,
            L("Optimize object rotation to have minimum amount of overhangs needing support "
              "structures.\nNote that this method will try to find the best surface of the object "
              "for touching the print bed if no elevation is set.")},
           // Just a min area bounding box that is done for all methods anyway.
           {L("Lowest Z height"),
            sla::find_min_z_height_rotation,
            L("Rotate the model to have the lowest z height for faster print time.")}};

    size_t m_method_id = 0;
    int    m_method_override = -1;
    float  m_accuracy  = 0.75;
    bool   m_use_explicit_selection = false;
    std::vector<size_t> m_explicit_objects;
    std::vector<size_t> m_limit_instance_ids;

    DynamicPrintConfig m_default_print_cfg;

    struct ObjRot
    {
        size_t               idx;
        std::optional<Vec2d> rot;
        ObjRot(size_t id): idx{id}, rot{} {}
    };

    std::vector<ObjRot> m_selected_object_ids;
    Plater *m_plater;

public:

    void prepare();
    void process(Ctl &ctl) override;

    RotoptimizeJob();
    // object_ids are model object indices. instance_ids limits which copies are rotated.
    RotoptimizeJob(std::vector<size_t> object_ids, std::vector<size_t> instance_ids, int method_id);

    void finalize(bool canceled, std::exception_ptr &) override;

    static constexpr size_t get_methods_count() { return std::size(Methods); }

    static std::string get_method_name(size_t i);

    static std::string get_method_description(size_t i);
};

}} // namespace Slic3r::GUI

#endif // ROTOPTIMIZEJOB_HPP
