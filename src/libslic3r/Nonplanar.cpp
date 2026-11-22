#include "Nonplanar.hpp"
#include "PlateInstanceCount.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "AABBTreeIndirect.hpp"
#include "BoundingBox.hpp"
#include "ClipperUtils.hpp"
#include "ExPolygon.hpp"
#include "ExtrusionEntity.hpp"
#include "Flow.hpp"
#include "Geometry.hpp"
#include "Layer.hpp"
#include "Model.hpp"
#include "Print.hpp"
#include "TriangleMesh.hpp"

namespace Slic3r {
namespace {

struct NonplanarSurface {
    indexed_triangle_set             its;
    AABBTreeIndirect::Tree<3, float> tree;
    float                            z_top{ 0.f };
    // Footprint of the skin attached to each object layer. Used again after ironing.
    std::vector<ExPolygons>          clip_by_layer;
};

std::mutex g_mutex;
std::unordered_map<const PrintObject *, std::shared_ptr<NonplanarSurface>> g_surfaces;

void forget_surface(const PrintObject &object)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_surfaces.erase(&object);
}

void store_surface(const PrintObject &object, std::shared_ptr<NonplanarSurface> surface)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (surface)
        g_surfaces[&object] = std::move(surface);
    else
        g_surfaces.erase(&object);
}

std::shared_ptr<NonplanarSurface> cached_surface(const PrintObject &object)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_surfaces.find(&object);
    return it == g_surfaces.end() ? nullptr : it->second;
}

std::shared_ptr<NonplanarSurface> build_surface(const PrintObject &object)
{
    indexed_triangle_set its = object.model_object()->raw_indexed_triangle_set();
    if (its.vertices.empty() || its.indices.empty())
        return nullptr;

    const Transform3d trafo = object.trafo_centered();
    for (stl_vertex &vertex : its.vertices)
        vertex = (trafo * vertex.cast<double>()).cast<float>().eval();
    if (trafo.linear().determinant() < 0.) {
        for (stl_triangle_vertex_indices &face : its.indices)
            std::swap(face[0], face[1]);
    }

    auto surface = std::make_shared<NonplanarSurface>();
    surface->z_top = its.vertices.front().z();
    for (const stl_vertex &vertex : its.vertices)
        surface->z_top = std::max(surface->z_top, vertex.z());
    surface->its = std::move(its);
    surface->tree = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(surface->its.vertices, surface->its.indices);
    surface->clip_by_layer.assign(object.layer_count(), {});
    return surface;
}

std::shared_ptr<NonplanarSurface> surface_for(const PrintObject &object)
{
    if (std::shared_ptr<NonplanarSurface> existing = cached_surface(object))
        return existing;
    std::shared_ptr<NonplanarSurface> built = build_surface(object);
    store_surface(object, built);
    return built;
}

Vec3f face_normal(const indexed_triangle_set &its, int face)
{
    const stl_triangle_vertex_indices indices = its.indices[face];
    const Vec3f a = its.vertices[indices[0]];
    const Vec3f b = its.vertices[indices[1]];
    const Vec3f c = its.vertices[indices[2]];
    const Vec3f normal = (b - a).cross(c - a);
    const float length = normal.norm();
    return length > 1e-8f ? Vec3f(normal / length) : Vec3f::Zero();
}

struct HitZ {
    double z;
    float  nz;
};

std::vector<HitZ> mesh_hits(const NonplanarSurface &surface, double x, double y)
{
    const Vec3d origin(x, y, double(surface.z_top) + 1.);
    const Vec3d dir(0., 0., -1.);
    std::vector<igl::Hit> hits;
    std::vector<HitZ> found;
    if (!AABBTreeIndirect::intersect_ray_all_hits(surface.its.vertices, surface.its.indices, surface.tree, origin, dir, hits))
        return found;
    found.reserve(hits.size());
    for (const igl::Hit &hit : hits) {
        if (hit.id < 0 || hit.id >= int(surface.its.indices.size()))
            continue;
        const float nz = face_normal(surface.its, hit.id).z();
        if (nz <= 0.05f)
            continue;
        found.push_back(HitZ{ double(origin.z()) + double(dir.z()) * double(hit.t), nz });
    }
    return found;
}

// Highest upward-facing hit. Used to decide whether the nozzle would strike taller material.
std::optional<double> mesh_z(const NonplanarSurface &surface, double x, double y)
{
    std::optional<double> best;
    for (const HitZ &hit : mesh_hits(surface, x, y))
        if (!best || hit.z > *best)
            best = hit.z;
    return best;
}

// Highest hit that belongs to this pass: at or below the pass top, and no deeper than max_depth.
std::optional<double> mesh_z_in_window(const NonplanarSurface &surface, double x, double y, double z_hi, double z_lo)
{
    std::optional<double> best;
    for (const HitZ &hit : mesh_hits(surface, x, y)) {
        if (hit.z > z_hi + 0.05 || hit.z < z_lo - 0.05)
            continue;
        if (!best || hit.z > *best)
            best = hit.z;
    }
    return best;
}

bool nozzle_clears(const NonplanarSurface &surface, const Vec3f &tip, double radius, double max_depth)
{
    const int samples = 8;
    for (int i = 0; i < samples; ++i) {
        const double angle = (2.0 * PI * i) / samples;
        const std::optional<double> z = mesh_z(surface, tip.x() + radius * std::cos(angle), tip.y() + radius * std::sin(angle));
        if (z && *z > double(tip.z()) + max_depth + 0.05)
            return false;
    }
    return true;
}

struct V3 {
    double x;
    double y;
    double z;
};

// Keep the part of a polygon inside a horizontal slab. The top edge is closed, the bottom edge is open
// so the next lower pass owns the shared boundary.
std::vector<V3> clip_slab(std::vector<V3> poly, double z_lo, double z_hi)
{
    auto clip_plane = [](const std::vector<V3> &input, double z, bool keep_below) {
        std::vector<V3> out;
        if (input.empty())
            return out;
        out.reserve(input.size() + 2);
        for (size_t i = 0; i < input.size(); ++i) {
            const V3 &a = input[i];
            const V3 &b = input[(i + 1) % input.size()];
            const bool a_in = keep_below ? a.z <= z : a.z >= z;
            const bool b_in = keep_below ? b.z <= z : b.z >= z;
            if (a_in && b_in) {
                out.push_back(b);
            } else if (a_in != b_in && std::abs(b.z - a.z) > 1e-9) {
                const double t = (z - a.z) / (b.z - a.z);
                const V3 mid{ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, z };
                out.push_back(mid);
                if (b_in)
                    out.push_back(b);
            }
        }
        return out;
    };
    poly = clip_plane(poly, z_hi, true);
    return clip_plane(poly, z_lo, false);
}

struct Band {
    int    layer_idx;
    double z_lo;
    double z_hi;
};

// Slabs as thick as the nozzle may drop. A lower layer is skipped while a higher
// slab already covers it, so the slope is one surface instead of a stack of flat skins.
std::vector<Band> layer_bands(const PrintObject &object, double max_depth)
{
    std::vector<Band> bands;
    const double z_shift = object.slicing_parameters().object_print_z_min;
    const int top = int(object.layer_count()) - 1;
    const double depth = std::max(max_depth, 0.05);
    double claimed_below = 1e9;
    for (int i = top; i >= 1; --i) {
        const Layer *layer = object.get_layer(i);
        const double z_hi = layer->print_z - z_shift + (i == top ? 0.05 : 0.0);
        if (z_hi >= claimed_below - 0.02)
            continue;
        bands.push_back(Band{ i, z_hi - depth, z_hi });
        claimed_below = z_hi - depth;
    }
    return bands;
}

Polylines lines_across(const ExPolygon &src, double angle_deg, double spacing_mm)
{
    if (spacing_mm < 0.05 || src.empty())
        return {};
    ExPolygon ex = src;
    ex.rotate(Geometry::deg2rad(-angle_deg));
    const BoundingBox bb = get_extents(ex);
    const coord_t step = std::max(coord_t(scale_(spacing_mm)), coord_t(1));
    Polylines grid;
    for (coord_t x = bb.min.x(); x <= bb.max.x(); x += step)
        grid.emplace_back(Polyline{ Point(x, bb.min.y() - step), Point(x, bb.max.y() + step) });
    Polylines clipped = intersection_pl(grid, ex);
    const double back = Geometry::deg2rad(angle_deg);
    for (Polyline &line : clipped)
        line.rotate(back);
    return clipped;
}

Polyline closed_contour(const Polygon &polygon)
{
    Polyline line(polygon.points);
    if (line.size() >= 2 && line.points.front() != line.points.back())
        line.points.push_back(line.points.front());
    return line;
}

void throw_if_canceled(const PrintObject &object)
{
    if (object.print()->canceled())
        throw CanceledException();
}

} // namespace

Geometry::ArcWelder::Path project_nonplanar_path(
    const Geometry::ArcWelder::Path &path,
    const PrintObject               &object,
    const Layer                     &layer,
    float                            extrusion_height)
{
    Geometry::ArcWelder::Path out;
    if (path.empty() || extrusion_height <= 1e-6f)
        return out;
    const std::shared_ptr<NonplanarSurface> surface = surface_for(object);
    if (surface == nullptr)
        return out;

    const double z_shift = object.slicing_parameters().object_print_z_min;
    const double depth = std::max(object.config().nonplanar_max_depth.value, 0.05);
    const double z_hi = layer.print_z - z_shift + (layer.upper_layer == nullptr ? 0.05 : 0.0);
    const double z_lo = z_hi - depth;
    const double step = scale_(0.2);
    float last_fraction = 1.f;
    auto push_point = [&](const Point &point, float e_fraction) {
        Geometry::ArcWelder::Segment segment;
        segment.point = point;
        segment.e_fraction = e_fraction;
        segment.height_fraction = last_fraction;
        if (const std::optional<double> z = mesh_z_in_window(*surface, unscale<double>(point.x()), unscale<double>(point.y()), z_hi, z_lo)) {
            const double target = std::clamp(*z + z_shift, z_lo + z_shift, layer.print_z);
            segment.height_fraction = float(1.0 + (target - layer.print_z) / double(extrusion_height));
            last_fraction = segment.height_fraction;
        }
        out.push_back(segment);
    };

    push_point(path.front().point, path.front().e_fraction);
    for (size_t i = 1; i < path.size(); ++i) {
        const Point from = out.back().point;
        const Point to = path[i].point;
        const double length = (to - from).cast<double>().norm();
        const int pieces = std::max(1, int(std::ceil(length / step)));
        for (int piece = 1; piece <= pieces; ++piece) {
            const double t = double(piece) / pieces;
            const Vec2d delta = (to - from).cast<double>() * t;
            const Point at(coord_t(std::llround(from.x() + delta.x())), coord_t(std::llround(from.y() + delta.y())));
            push_point(piece == pieces ? to : at, path[i].e_fraction);
        }
    }
    return out;
}

void clip_nonplanar_overlaps(PrintObject &object)
{
    const std::shared_ptr<NonplanarSurface> surface = cached_surface(object);
    if (surface == nullptr)
        return;
    const size_t count = std::min(surface->clip_by_layer.size(), object.layer_count());
    for (size_t layer_idx = 0; layer_idx < count; ++layer_idx) {
        const ExPolygons &cut = surface->clip_by_layer[layer_idx];
        if (cut.empty())
            continue;
        Layer *layer = object.get_layer(int(layer_idx));
        for (size_t region_id = 0; region_id < layer->region_count(); ++region_id)
            layer->get_region(int(region_id))->clip_top_solid_infill(cut);
    }
}

void apply_nonplanar_surfaces(PrintObject &object)
{
    forget_surface(object);
    if (!object.config().nonplanar_slicing.value)
        return;
    // Dipping the nozzle is only reliable with one object on the plate.
    // Sequential printing does not lift this: the other objects are still there.
    size_t copies = print_plate_instances(object.print());
    if (copies == 0)
        for (const PrintObject *other : object.print()->objects())
            copies += other->instances().size();
    if (copies > 1)
        return;
    const double max_depth = object.config().nonplanar_max_depth.value;
    if (max_depth <= 0.05 || object.layer_count() < 2)
        return;
    const std::shared_ptr<NonplanarSurface> surface = build_surface(object);
    if (surface == nullptr)
        return;

    const double nozzle = object.print()->config().nozzle_diameter.values.empty()
        ? 0.4
        : object.print()->config().nozzle_diameter.values.front();
    // Collision is checked about one nozzle width out, so a gentle slope is not rejected
    // just because the crown of the same surface is higher further away.
    const double clearance_radius = std::max(nozzle, 0.4);
    const double min_nz = clearance_radius / std::sqrt(clearance_radius * clearance_radius + max_depth * max_depth);
    const std::vector<Band> bands = layer_bands(object, max_depth);
    if (bands.empty())
        return;

    std::vector<char> vertex_clears(surface->its.vertices.size(), -1);
    auto clears = [&](int index) {
        if (vertex_clears[index] < 0)
            vertex_clears[index] = nozzle_clears(*surface, surface->its.vertices[index], clearance_radius, max_depth) ? 1 : 0;
        return vertex_clears[index] == 1;
    };

    struct Patch {
        Polygon polygon;
        float   nz;
    };
    std::vector<std::vector<Patch>> by_layer(object.layer_count());
    const auto &vertices = surface->its.vertices;
    for (const stl_triangle_vertex_indices &face : surface->its.indices) {
        const Vec3f a = vertices[face[0]];
        const Vec3f b = vertices[face[1]];
        const Vec3f c = vertices[face[2]];
        const Vec3f normal = (b - a).cross(c - a);
        const float length = normal.norm();
        const float nz = length > 1e-8f ? normal.z() / length : 0.f;
        if (nz < float(min_nz))
            continue;
        if (!clears(face[0]) || !clears(face[1]) || !clears(face[2]))
            continue;

        const float zmin = std::min(a.z(), std::min(b.z(), c.z()));
        const float zmax = std::max(a.z(), std::max(b.z(), c.z()));
        const std::array<V3, 3> corners{{
            V3{ double(a.x()), double(a.y()), double(a.z()) },
            V3{ double(b.x()), double(b.y()), double(b.z()) },
            V3{ double(c.x()), double(c.y()), double(c.z()) }
        }};
        for (const Band &band : bands) {
            if (double(zmax) < band.z_lo || double(zmin) > band.z_hi)
                continue;
            const std::vector<V3> clipped = clip_slab(std::vector<V3>(corners.begin(), corners.end()), band.z_lo, band.z_hi);
            if (clipped.size() < 3)
                continue;
            Points points;
            points.reserve(clipped.size());
            for (const V3 &vertex : clipped)
                points.push_back(Point::new_scale(vertex.x, vertex.y));
            Polygon polygon(points);
            if (!polygon.is_valid() || std::abs(polygon.area()) < scale_(0.02) * scale_(0.02))
                continue;
            by_layer[band.layer_idx].push_back(Patch{ std::move(polygon), nz });
        }
    }

    for (size_t layer_idx = 0; layer_idx < by_layer.size(); ++layer_idx) {
        if (by_layer[layer_idx].empty())
            continue;
        throw_if_canceled(object);
        Layer *layer = object.get_layer(int(layer_idx));
        if (layer->region_count() == 0)
            continue;

        Polygons footprints;
        footprints.reserve(by_layer[layer_idx].size());
        float slope_nz = 1.f;
        for (const Patch &patch : by_layer[layer_idx]) {
            footprints.push_back(patch.polygon);
            slope_nz = std::min(slope_nz, patch.nz);
        }
        const float smooth = float(scale_(0.2));
        ExPolygons region = closing_ex(union_ex(footprints), smooth);
        if (region.empty())
            continue;

        const LayerRegion &sample = *layer->get_region(0);
        const Flow flow = sample.flow(frTopSolidInfill);
        if (flow.spacing() <= 0.f)
            continue;
        const double angle = sample.region().config().fill_angle.value;
        ExtrusionAttributes attributes(ExtrusionRole::TopSolidInfill, flow);
        attributes.nonplanar = true;
        // On a slope, XY spacing has to shrink with the surface angle or the beads pull apart.
        const double spacing = std::max(0.12, double(flow.spacing()) * std::clamp(double(slope_nz), 0.35, 1.0));
        const ExPolygons infill_area = offset_ex(region, -float(scale_(0.45 * spacing)));

        ExtrusionPaths paths;
        auto add_line = [&](Polyline line) {
            if (line.size() >= 2 && line.length() >= scale_(0.3))
                paths.emplace_back(std::move(line), attributes);
        };
        for (const ExPolygon &island : region) {
            if (std::abs(island.area()) < scale_(0.4) * scale_(0.4))
                continue;
            Polygon contour = island.contour;
            contour.douglas_peucker(scale_(0.05));
            if (contour.size() >= 3)
                add_line(closed_contour(contour));
            for (const Polygon &hole : island.holes) {
                if (std::abs(hole.area()) < scale_(0.4) * scale_(0.4))
                    continue;
                Polygon opened = hole;
                opened.douglas_peucker(scale_(0.05));
                if (opened.size() >= 3)
                    add_line(closed_contour(opened));
            }
        }
        for (const ExPolygon &island : infill_area.empty() ? region : infill_area) {
            if (std::abs(island.area()) < scale_(0.4) * scale_(0.4))
                continue;
            for (Polyline &line : lines_across(island, angle, spacing))
                add_line(std::move(line));
            // A second direction closes the gaps a single hatch leaves on a curve.
            if (slope_nz < 0.92f)
                for (Polyline &line : lines_across(island, angle + 90.0, spacing))
                    add_line(std::move(line));
        }
        if (paths.empty())
            continue;

        // Remove the flat top-solid stairs under this skin. Leave the walls so it stays supported.
        const double z_shift = object.slicing_parameters().object_print_z_min;
        const double cover_lo = layer->print_z - z_shift - std::max(max_depth, 0.05);
        for (int covered = int(layer_idx); covered >= 1; --covered) {
            Layer *lower = object.get_layer(covered);
            if (covered != int(layer_idx) && lower->print_z < cover_lo + z_shift - 0.02)
                break;
            if (size_t(covered) < surface->clip_by_layer.size())
                surface->clip_by_layer[covered] = union_ex(surface->clip_by_layer[covered], region);
            for (size_t region_id = 0; region_id < lower->region_count(); ++region_id)
                lower->get_region(int(region_id))->clip_top_solid_infill(region);
        }
        layer->get_region(0)->append_nonplanar_fills(std::move(paths));
    }

    store_surface(object, surface);
}

} // namespace Slic3r
