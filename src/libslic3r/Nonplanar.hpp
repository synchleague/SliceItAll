#ifndef slic3r_Nonplanar_hpp_
#define slic3r_Nonplanar_hpp_

#include "Geometry/ArcWelder.hpp"

namespace Slic3r {

class Layer;
class PrintObject;

// Replace stair-stepped top solid infill with toolpaths that follow the mesh,
// limited by nonplanar_max_depth.
void apply_nonplanar_surfaces(PrintObject &object);

// Drop top solid and ironing that overlap a non-planar skin. Safe when the option is off.
void clip_nonplanar_overlaps(PrintObject &object);

// Sample a fitted toolpath onto the mesh. Empty when the input is empty.
Geometry::ArcWelder::Path project_nonplanar_path(
    const Geometry::ArcWelder::Path &path,
    const PrintObject               &object,
    const Layer                     &layer,
    float                            extrusion_height);

} // namespace Slic3r

#endif
