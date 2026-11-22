#ifndef slic3r_PlateInstanceCount_hpp_
#define slic3r_PlateInstanceCount_hpp_

#include <cstddef>

namespace Slic3r {

class Print;

// How many printable instances are on the bed about to be applied, counted
// before "slice the bed as one object" merges them into a single model.
void note_bed_instances_for_apply(size_t count);
size_t bed_instances_for_apply();

void set_print_plate_instances(const Print *print, size_t count);
size_t print_plate_instances(const Print *print);

} // namespace Slic3r

#endif
