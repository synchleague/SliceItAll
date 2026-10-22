#include "MultipleBeds.hpp"

#include "BuildVolume.hpp"
#include "Model.hpp"
#include "Print.hpp"

#include <cassert>
#include <algorithm>

namespace Slic3r {

MultipleBeds s_multiple_beds;
bool s_reload_preview_after_switching_beds = false;
bool s_beds_just_switched = false;
bool s_beds_switched_since_last_gcode_load = false;

bool is_sliceable(const PrintStatus status) {
    if (status == PrintStatus::empty) {
        return false;
    }
    if (status == PrintStatus::invalid) {
        return false;
    }
    if (status == PrintStatus::outside) {
        return false;
    }
    return true;
}

namespace BedsGrid {
Index grid_coords_abs2index(GridCoords coords) {
    coords = {std::abs(coords.x()), std::abs(coords.y())};

    const int x{coords.x() + 1};
    const int y{coords.y() + 1};
    const int a{std::max(x, y)};

    if (x == a && y == a) {
        return a*a - 1;
    } else if (x == a) {
        return a*a - 2 * (a - 1) + coords.y() - 1;
    } else {
        assert(y == a);
        return a*a - (a - 1) + coords.x() - 1;
    }
}

const int quadrant_offset{std::numeric_limits<int>::max() / 4};

Index grid_coords2index(const GridCoords &coords) {
    const int index{grid_coords_abs2index(coords)};

    if (index >= quadrant_offset) {
        throw std::runtime_error("Object is too far from center!");
    }

    if (coords.x() >= 0 && coords.y() >= 0) {
        return index;
    } else if (coords.x() >= 0 && coords.y() < 0) {
        return quadrant_offset + index;
    } else if (coords.x() < 0 && coords.y() >= 0) {
        return 2*quadrant_offset + index;
    } else {
        return 3*quadrant_offset + index;
    }
}

GridCoords index2grid_coords(Index index) {
    if (index < 0) {
        throw std::runtime_error{"Negative bed index cannot be translated to coords!"};
    }

    const int quadrant{index / quadrant_offset};
    index = index % quadrant_offset;

    GridCoords result{GridCoords::Zero()};
    if (index == 0) {
        return result;
    }

    int id = index;
    ++id;
    int a = 1;
    while ((a+1)*(a+1) < id)
        ++a;
    id = id - a*a;
    result.x()=a;
    result.y()=a;
    if (id <= a)
        result.y() = id-1;
    else
        result.x() = id-a-1;

    if (quadrant == 1) {
        result.y() = -result.y();
    } else if (quadrant == 2) {
        result.x() = -result.x();
    } else if (quadrant == 3) {
        result.y() = -result.y();
        result.x() = -result.x();
    } else if (quadrant != 0){
        throw std::runtime_error{"Impossible bed index > max int!"};
    }
    return result;
}
}

Vec3d MultipleBeds::get_bed_translation(int id) const
{
    if (id == 0)
        return Vec3d::Zero();
    int x = 0;
    int y = 0;
    if (m_legacy_layout)
        x = id;
    else {
        BedsGrid::GridCoords coords{BedsGrid::index2grid_coords(id)};
        x = coords.x();
        y = coords.y();
    }

    // As for the m_legacy_layout switch, see comments at definition of bed_gap_relative.
    Vec2d  gap = bed_gap();
    double gap_x = (m_legacy_layout ? m_build_volume_bb.size().x() * (2./10.) : gap.x());
    return Vec3d(x * (m_build_volume_bb.size().x() + gap_x),
                 y * (m_build_volume_bb.size().y() + gap.y()), // When using legacy layout, y is zero anyway.
                 0.);
}

void MultipleBeds::ensure_beds(int count)
{
    if (count < 1)
        return;
    const size_t n = size_t(count);
    if (m_bed_locked.size() < n)
        m_bed_locked.resize(n, 0);
    if (m_occupied_beds_cache.size() < n)
        m_occupied_beds_cache.resize(n, 0);
    if (s_print_statuses.size() < n)
        s_print_statuses.resize(n, PrintStatus::idle);
    if (s_bed_selector_thumbnail_changed.size() < n)
        s_bed_selector_thumbnail_changed.resize(n, 0);
}

void MultipleBeds::clear_inst_map()
{
    m_inst_to_bed.clear();
    std::fill(m_occupied_beds_cache.begin(), m_occupied_beds_cache.end(), 0);
}

void MultipleBeds::set_instance_bed(ObjectID id, bool printable, int bed_idx)
{
    if (bed_idx < 0)
        return;
    ensure_beds(bed_idx + 1);
    m_inst_to_bed[id] = bed_idx;

    if (printable)
        m_occupied_beds_cache[bed_idx] = true;
}

void MultipleBeds::inst_map_updated()
{
    int max_bed_idx = 0;
    for (const auto& [obj_id, bed_idx] : m_inst_to_bed)
        max_bed_idx = std::max(max_bed_idx, bed_idx);

    // Occupied plates stay. An empty plate added from the strip stays too,
    // because add_bed() raises m_minimum_beds and this used to overwrite it.
    const int occupied = max_bed_idx + 1;
    if (occupied > m_minimum_beds)
        m_minimum_beds = occupied;

    const int previous = m_number_of_beds;
    const int needed = std::max(m_minimum_beds, occupied);
    ensure_beds(needed);
    if (m_number_of_beds != needed) {
        m_number_of_beds = needed;
        if (needed > previous)
            m_active_bed = m_number_of_beds - 1;
        request_next_bed(false);
    }
    if (m_active_bed >= m_number_of_beds)
        m_active_bed = m_number_of_beds - 1;
}

void MultipleBeds::request_next_bed(bool show)
{
    m_show_next_bed = show;
}

bool MultipleBeds::add_bed()
{
    m_minimum_beds = m_number_of_beds + 1;
    m_number_of_beds = m_minimum_beds;
    ensure_beds(m_number_of_beds);
    request_next_bed(false);
    return true;
}

void MultipleBeds::reset_to_first_bed()
{
    m_number_of_beds = 1;
    m_minimum_beds = 1;
    m_active_bed = 0;
    std::fill(m_bed_locked.begin(), m_bed_locked.end(), 0);
    m_show_next_bed = false;
    clear_inst_map();
}

bool MultipleBeds::remove_bed(int bed)
{
    if (m_number_of_beds <= 1 || bed < 0 || bed >= m_number_of_beds)
        return false;
    for (int i = bed; i < m_number_of_beds - 1; ++i)
        m_bed_locked[i] = m_bed_locked[i + 1];
    m_bed_locked[m_number_of_beds - 1] = false;
    if (m_minimum_beds > 1)
        --m_minimum_beds;
    --m_number_of_beds;
    if (m_active_bed == bed)
        m_active_bed = std::max(0, bed - 1);
    else if (m_active_bed > bed)
        --m_active_bed;
    if (m_active_bed >= m_number_of_beds)
        m_active_bed = m_number_of_beds - 1;
    return true;
}

bool MultipleBeds::is_bed_locked(int bed) const
{
    return bed >= 0 && bed < int(m_bed_locked.size()) && m_bed_locked[size_t(bed)];
}

void MultipleBeds::set_bed_locked(int bed, bool locked)
{
    if (bed < 0)
        return;
    ensure_beds(bed + 1);
    m_bed_locked[size_t(bed)] = locked ? 1 : 0;
}

void MultipleBeds::capture_bed_locks(Model& model) const
{
    const int count = std::max(0, m_number_of_beds);
    model.bed_locks_known = true;
    model.bed_count = count;
    model.bed_locked.assign(size_t(count), 0);
    for (int i = 0; i < count && i < int(m_bed_locked.size()); ++i)
        model.bed_locked[size_t(i)] = m_bed_locked[size_t(i)] ? 1 : 0;
}

void MultipleBeds::restore_bed_locks(const Model& model)
{
    if (!model.bed_locks_known)
        return;
    int beds = std::max(m_number_of_beds, std::max(model.bed_count, 1));
    for (size_t i = 0; i < model.bed_locked.size(); ++i)
        if (model.bed_locked[i])
            beds = std::max(beds, int(i) + 1);
    m_number_of_beds = beds;
    m_minimum_beds = std::max(m_minimum_beds, beds);
    ensure_beds(beds);
    std::fill(m_bed_locked.begin(), m_bed_locked.end(), 0);
    for (size_t i = 0; i < model.bed_locked.size() && i < m_bed_locked.size(); ++i)
        m_bed_locked[i] = model.bed_locked[i] ? 1 : 0;
}

void MultipleBeds::set_active_bed(int i)
{
    if (i >= 0 && i < m_number_of_beds) {
        ensure_beds(m_number_of_beds);
        m_active_bed = i;
    }
}

namespace MultipleBedsUtils {
InstanceOffsets get_instance_offsets(Model& model) {
    InstanceOffsets result;
    for (ModelObject* mo : model.objects) {
        for (ModelInstance* mi : mo->instances) {
            result.emplace_back(mi->get_offset());
        }
    }
    return result;
}

ObjectInstances get_object_instances(const Model& model) {
    ObjectInstances result;

    std::transform(
        model.objects.begin(),
        model.objects.end(),
        std::back_inserter(result),
        [](ModelObject *object){
            return std::pair{object, object->instances};
        }
    );

    return result;
}

void restore_instance_offsets(Model& model, const InstanceOffsets &offsets)
{
    size_t i = 0;
    for (ModelObject* mo : model.objects) {
        for (ModelInstance* mi : mo->instances) {
            mi->set_offset(offsets[i++]);
        }
    }
}

void restore_object_instances(Model& model, const ObjectInstances &object_instances) {
    ModelObjectPtrs objects;

    std::transform(
        object_instances.begin(),
        object_instances.end(),
        std::back_inserter(objects),
        [](const std::pair<ModelObject *, ModelInstancePtrs> &key_value){
            auto [object, instances]{key_value};
            object->instances = std::move(instances);
            return object;
        }
    );

    model.objects = objects;
}

void with_single_bed_model_fff(Model &model, const int bed_index, const std::function<void()> &callable, bool slice_as_one_object) {
    const InstanceOffsets original_offssets{MultipleBedsUtils::get_instance_offsets(model)};
    const ObjectInstances original_objects{get_object_instances(model)};
    const int original_bed{s_multiple_beds.get_active_bed()};
    ModelObject* merged_view = nullptr;
    Slic3r::ScopeGuard guard([&]() {
        restore_object_instances(model, original_objects);
        restore_instance_offsets(model, original_offssets);
        s_multiple_beds.set_active_bed(original_bed);
        ModelObject::release(merged_view);
    });

    s_multiple_beds.move_from_bed_to_first_bed(model, bed_index);
    s_multiple_beds.remove_instances_outside_outside_bed(model, bed_index);
    s_multiple_beds.set_active_bed(bed_index);
    if (slice_as_one_object) {
        merged_view = ModelObject::merged_for_slicing(model, bed_index);
        if (merged_view)
            model.objects = { merged_view };
    }
    callable();
}

using InstancesPrintability = std::vector<bool>;

InstancesPrintability get_instances_printability(const Model &model) {
    InstancesPrintability result;
    for (ModelObject* mo : model.objects) {
        for (ModelInstance* mi : mo->instances) {
            result.emplace_back(mi->printable);
        }
    }
    return result;
}

void restore_instances_printability(Model& model, const InstancesPrintability &printability)
{
    size_t i = 0;
    for (ModelObject* mo : model.objects) {
        for (ModelInstance* mi : mo->instances) {
            mi->printable = printability[i++];
        }
    }
}

void with_single_bed_model_sla(Model &model, const int bed_index, const std::function<void()> &callable) {
    const InstanceOffsets original_offssets{get_instance_offsets(model)};
    const InstancesPrintability original_printability{get_instances_printability(model)};
    const int original_bed{s_multiple_beds.get_active_bed()};
    Slic3r::ScopeGuard guard([&]() {
        restore_instance_offsets(model, original_offssets);
        restore_instances_printability(model, original_printability);
        s_multiple_beds.set_active_bed(original_bed);
    });

    s_multiple_beds.move_from_bed_to_first_bed(model, bed_index);
    s_multiple_beds.set_instances_outside_outside_bed_unprintable(model, bed_index);
    s_multiple_beds.set_active_bed(bed_index);
    callable();
}

}

bool MultipleBeds::is_instance_on_bed(const ObjectID id, const int bed_index) const
{
    auto it = m_inst_to_bed.find(id);
    return (it != m_inst_to_bed.end() && it->second == bed_index);
}

void MultipleBeds::remove_instances_outside_outside_bed(Model& model, const int bed_index) const {
    for (ModelObject* mo : model.objects) {
        mo->instances.erase(std::remove_if(
            mo->instances.begin(),
            mo->instances.end(),
            [&](const ModelInstance* instance){
                return !this->is_instance_on_bed(instance->id(), bed_index);
            }
        ), mo->instances.end());
    }

    model.objects.erase(std::remove_if(
        model.objects.begin(),
        model.objects.end(),
        [](const ModelObject *object){
            return object->instances.empty();
        }
    ), model.objects.end());
}

void MultipleBeds::set_instances_outside_outside_bed_unprintable(Model& model, const int bed_index) const {
    for (ModelObject* mo : model.objects) {
        for (ModelInstance* mi : mo->instances) {
            if (!this->is_instance_on_bed(mi->id(), bed_index)) {
                mi->printable = false;
            }
        }
    }
}

void MultipleBeds::move_from_bed_to_first_bed(Model& model, const int bed_index) const
{
    if (bed_index < 0)
        return;
    for (ModelObject* mo : model.objects) {
        for (ModelInstance* mi : mo->instances) {
            if (this->is_instance_on_bed(mi->id(), bed_index)) {
                mi->set_offset(mi->get_offset() - get_bed_translation(bed_index));
            }
        }
    }
}

bool MultipleBeds::is_glvolume_on_thumbnail_bed(const Model& model, int obj_idx, int instance_idx) const
{
    if (m_bed_for_thumbnails_generation == -2) {
        // Called from shape gallery, just render everything.
        return true;
    }

    if (obj_idx < 0 || instance_idx < 0 || obj_idx >= int(model.objects.size()) || instance_idx >= int(model.objects[obj_idx]->instances.size()))
        return false;

    auto it = m_inst_to_bed.find(model.objects[obj_idx]->instances[instance_idx]->id());
    if (it == m_inst_to_bed.end())
        return false;
    return (m_bed_for_thumbnails_generation < 0 || it->second == m_bed_for_thumbnails_generation);
}

int MultipleBeds::discover_highest_bed(Model& model, const BuildVolume& build_volume)
{
    int probe = std::max(m_number_of_beds, 1);
    int highest = 0;
    for (int pass = 0; pass < 24; ++pass) {
        // object_state also tests one plate past the current count.
        m_number_of_beds = probe;
        ensure_beds(probe + 1);
        model.update_print_volume_state(build_volume);
        highest = 0;
        for (const auto& [oid, bed_id] : m_inst_to_bed)
            highest = std::max(highest, bed_id);
        // Keep going while the last plate in this window is in use. A gap before
        // that plate means nothing sits further out.
        if (highest + 1 < probe)
            break;
        if (probe >= (1 << 20))
            break;
        probe *= 2;
    }
    return highest;
}

void MultipleBeds::update_shown_beds(Model& model, const BuildVolume& build_volume, bool only_remove /*=false*/) {
    const int original_number_of_beds = m_number_of_beds;
    const int stash_active = get_active_bed();
    if (! only_remove)
        discover_highest_bed(model, build_volume);
    else
        model.update_print_volume_state(build_volume);
    const int max_bed{std::accumulate(
        this->m_inst_to_bed.begin(), this->m_inst_to_bed.end(), 0,
        [](const int max_so_far, const std::pair<ObjectID, int> &value){
            return std::max(max_so_far, value.second);
        }
    )};
    m_number_of_beds = std::max(m_minimum_beds, max_bed + 1);
    ensure_beds(m_number_of_beds);
    if (m_number_of_beds > m_minimum_beds)
        m_minimum_beds = m_number_of_beds;
    model.update_print_volume_state(build_volume);
    set_active_bed(m_number_of_beds != original_number_of_beds ? 0 : stash_active);
    if (m_number_of_beds != original_number_of_beds)
        request_next_bed(false);
}

bool MultipleBeds::rearrange_after_load(Model& model, const BuildVolume& build_volume)
{
    int original_number_of_beds = m_number_of_beds;
    int stash_active = get_active_bed();
    Slic3r::ScopeGuard guard([&]() {
        m_legacy_layout = false;
        const int max_bed = discover_highest_bed(model, build_volume);
        m_number_of_beds = std::max(1, max_bed + 1);
        ensure_beds(m_number_of_beds);
        model.update_print_volume_state(build_volume);
        request_next_bed(false);
        set_active_bed(m_number_of_beds != original_number_of_beds ? 0 : stash_active);
        if (m_number_of_beds != original_number_of_beds)
            request_next_bed(false);
    });

    m_legacy_layout = true;
    int abs_max = std::max(m_number_of_beds, 1);
    for (int pass = 0; pass < 24; ++pass) {
        // Objects on a legacy linear layout can sit past the plates we have so far.
        m_number_of_beds = abs_max;
        ensure_beds(abs_max + 1);
        model.update_print_volume_state(build_volume);
        int max_bed = 0;
        for (const auto& [oid, bed_id] : m_inst_to_bed)
            max_bed = std::max(bed_id, max_bed);
        if (max_bed + 1 < abs_max)
            break;
        if (abs_max >= (1 << 20))
            break;
        abs_max *= 2;
    }
    m_number_of_beds = 1;
    m_legacy_layout = false;

    int max_bed = 0;

    // Check that no instances are out of any bed.
    std::map<ObjectID, std::pair<ModelInstance*, int>> id_to_ptr_and_bed;
    for (ModelObject* mo : model.objects) {
        for (ModelInstance* mi : mo->instances) {
            auto it = m_inst_to_bed.find(mi->id());
            if (it == m_inst_to_bed.end()) {
                // An instance is outside. Do not rearrange anything,
                // that could create collisions.
                return false;
            }
            id_to_ptr_and_bed[mi->id()] = std::make_pair(mi, it->second);
            max_bed = std::max(max_bed, it->second);
        }
    }

    // Now do the rearrangement
    m_number_of_beds = std::max(1, max_bed + 1);
    ensure_beds(m_number_of_beds);
    if (m_number_of_beds == 1)
        return false;

    // All instances are on some bed, at least two are used.
    // Move everything as if its bed was in the first position.
    for (auto& [oid, mi_and_bed] : id_to_ptr_and_bed) {
        auto& [mi, bed_idx] = mi_and_bed;
        m_legacy_layout = true;
        mi->set_offset(mi->get_offset() - get_bed_translation(bed_idx));
        m_legacy_layout = false;
        mi->set_offset(mi->get_offset() + get_bed_translation(bed_idx));
    }
    return true;
}



Vec2d MultipleBeds::bed_gap() const
{
    // This is the only function that defines how far apart should the beds be. Used in scene and arrange.
    // Note that the spacing is momentarily switched to legacy value of 2/10 when a project is loaded.
    // Slicers before 2.9.0 used this value for arrange, and there are existing projects with objects spaced that way (controlled by the m_legacy_layout flag).
    
    // TOUCHING THIS WILL BREAK LOADING OF EXISTING PROJECTS !!!

    double gap = std::min(100., m_build_volume_bb.size().norm() * (3./10.));
    return Vec2d::Ones() * gap;
}

bool MultipleBeds::is_bed_occupied(int i) const
{
    if (i < 0 || i >= int(m_occupied_beds_cache.size()))
        return false;
    return m_occupied_beds_cache[i] != 0;
}


Vec2crd MultipleBeds::get_bed_gap() const {
    return scaled(Vec2d{bed_gap() / 2.0});
};

void MultipleBeds::ensure_wipe_towers_on_beds(Model& model, const std::vector<std::unique_ptr<Print>>& prints)
{
    model.ensure_beds(get_number_of_beds());
    for (size_t bed_idx = 0; bed_idx < size_t(get_number_of_beds()) && bed_idx < prints.size(); ++bed_idx) {
        ModelWipeTower& mwt = model.get_wipe_tower_vector()[bed_idx];
        double depth = prints[bed_idx]->wipe_tower_data().depth;
        double width = prints[bed_idx]->wipe_tower_data().width;
        double brim  = prints[bed_idx]->wipe_tower_data().brim_width;

        Polygon plg(Points{Point::new_scale(-brim,-brim), Point::new_scale(brim+width, -brim), Point::new_scale(brim+width, brim+depth), Point::new_scale(-brim, brim+depth)});
        plg.rotate(Geometry::deg2rad(mwt.rotation));
        plg.translate(scaled(mwt.position));
        if (std::all_of(plg.points.begin(), plg.points.end(), [this](const Point& pt) { return !m_build_volume_bb.contains(unscale(pt)); }))
            mwt.position = 2*brim*Vec2d(1.,1.);
    }
}

#ifdef SLIC3R_GUI

void MultipleBeds::start_autoslice(std::function<void(int, bool)> select_bed_fn)
{
    if (is_autoslicing())
        return;
    m_select_bed_fn = select_bed_fn;

    m_autoslicing_original_bed = get_active_bed();

    m_autoslicing = true;
}



bool MultipleBeds::stop_autoslice(bool restore_original)
{
    if (! is_autoslicing())
        return false;
    m_autoslicing = false;

    if (restore_original)
        m_select_bed_fn(m_autoslicing_original_bed, false);
    return true;
}



void MultipleBeds::autoslice_next_bed()
{
    if (! is_autoslicing())
        return;
    int next_bed = s_multiple_beds.get_active_bed() + 1;
    if (next_bed >= s_multiple_beds.get_number_of_beds())
        next_bed = 0;
    m_select_bed_fn(next_bed, false);
}
#endif // SLIC3R_GUI


}

