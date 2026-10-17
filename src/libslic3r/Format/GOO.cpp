///|/ Copyright (c) 2024 Felix Reißmann @Felix-Rm
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "GOO.hpp"

#include "libslic3r/Exception.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/Time.hpp"
#include "libslic3r/libslic3r.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <string>

namespace Slic3r {

namespace {

template<typename T>
static T hton(T value)
{
    auto *bytes = reinterpret_cast<uint8_t *>(&value);
    for (size_t i = 0; i < sizeof(T) / 2; ++i)
        std::swap(bytes[i], bytes[sizeof(T) - 1 - i]);
    return value;
}

static float constrain_float(float value, float min_value, float max_value)
{
    return std::max(min_value, std::min(value, max_value));
}

static float constrained_map(float value, float min_value, float max_value, float out_min, float out_max)
{
    value = constrain_float(value, min_value, max_value);
    const float span = max_value - min_value;
    if (span <= 0.f)
        return out_max;
    const float t = (value - min_value) / span;
    return out_min * (1.f - t) + out_max * t;
}

// Layer numbers are 1-based. Bottom layers keep the initial exposure, then
// exposure fades across faded_layers down to the normal exposure.
static float layer_exposure_time(int layer_number, int bottom_layers, int faded_layers,
                                 float initial_exposure_time, float exposure_time)
{
    if (layer_number <= bottom_layers)
        return initial_exposure_time;
    return constrained_map(float(layer_number - bottom_layers), 1.f, float(faded_layers) + 1.f,
                           initial_exposure_time, exposure_time);
}

static void copy_fixed(char *buffer, size_t size, const std::string &value)
{
    if (size == 0)
        return;
    std::fill_n(buffer, size, '\0');
    const size_t count = std::min(size - 1, value.size());
    std::copy_n(value.data(), count, buffer);
}

static void write_thumbnail(size_t width, size_t height, const ThumbnailsList &thumbnails, uint16_t *buffer)
{
    if (thumbnails.empty() || width == 0 || height == 0)
        return;

    auto thumbnail = std::find_if(thumbnails.begin(), thumbnails.end(), [&](const ThumbnailData &item) {
        return item.width == width && item.height == height;
    });
    if (thumbnail == thumbnails.end())
        thumbnail = thumbnails.end() - 1;
    if (!thumbnail->is_valid() || thumbnail->pixels.size() < size_t(thumbnail->width) * thumbnail->height * 4)
        return;

    const size_t thumb_x_max = thumbnail->width - 1;
    const size_t thumb_y_max = thumbnail->height - 1;
    const size_t thumb_scaling = std::max(thumb_x_max, thumb_y_max);

    for (size_t y = 0; y < height; ++y) {
        for (size_t x = 0; x < width; ++x) {
            const double x_norm = double(x) / double(width - 1) - 0.5;
            const double y_norm = double(y) / double(height - 1) - 0.5;
            const int from_x = int(std::lround(thumb_x_max / 2.0 + x_norm * thumb_scaling));
            const int from_y = int(std::lround(thumb_y_max / 2.0 + y_norm * thumb_scaling));
            if (from_x < 0 || size_t(from_x) > thumb_x_max || from_y < 0 || size_t(from_y) > thumb_y_max)
                continue;

            const size_t from_idx = (size_t(from_y) * thumbnail->width + size_t(from_x)) * 4;
            const uint16_t r = thumbnail->pixels[from_idx] >> 3;
            const uint16_t g = thumbnail->pixels[from_idx + 1] >> 2;
            const uint16_t b = thumbnail->pixels[from_idx + 2] >> 3;
            const uint16_t pixel = (r << 11) | (g << 5) | b;
            buffer[(height - 1 - y) * width + x] = uint16_t((pixel << 8) | (pixel >> 8));
        }
    }
}

#if defined(_MSC_VER)
#define GOO_PACK_PUSH __pragma(pack(push, 1))
#define GOO_PACK_POP __pragma(pack(pop))
#define GOO_PACK_ATTR
#else
#define GOO_PACK_PUSH
#define GOO_PACK_POP
#define GOO_PACK_ATTR [[gnu::packed]]
#endif

// Every multi-byte field is stored big-endian. Single bytes are left as written.
// The version tag is lowercase. That is the string the Mars 4 Max writer was tested with.
GOO_PACK_PUSH
struct GOO_PACK_ATTR header_info
{
    char version[4] = {'v', '3', '.', '0'};
    uint8_t magic_tag[8] = {0x07, 0x00, 0x00, 0x00, 0x44, 0x4c, 0x50, 0x00};
    char software_info[32]{};
    char software_version[24]{};
    char file_time[24]{};
    char printer_name[32]{};
    char printer_type[32]{};
    char resin_profile_name[32]{};
    uint16_t aa_level;
    uint16_t grey_level;
    uint16_t blur_level;
    uint16_t small_preview[116 * 116]{};
    uint8_t delimiter_1[2] = {0x0d, 0x0a};
    uint16_t big_preview[290 * 290]{};
    uint8_t delimiter_2[2] = {0x0d, 0x0a};
    uint32_t total_layers;
    uint16_t x_resolution;
    uint16_t y_resolution;
    uint8_t x_mirror;
    uint8_t y_mirror;
    float x_platform_size_mm;
    float y_platform_size_mm;
    float z_platform_size_mm;
    float layer_thickness_mm;
    float common_exposure_time_s;
    bool exposure_delivery_time_static;
    float turn_off_time_s;
    float bottom_before_lift_time_s;
    float bottom_after_lift_time_s;
    float bottom_after_retract_time_s;
    float before_lift_time_s;
    float after_lift_time_s;
    float after_retract_time_s;
    float bottom_exposure_time_s;
    uint32_t bottom_layers;
    float bottom_lift_distance_mm;
    float bottom_lift_speed_mm_min;
    float lift_distance_mm;
    float lift_speed_mm_min;
    float bottom_retract_distance_mm;
    float bottom_retract_speed_mm_min;
    float retract_distance_mm;
    float retract_speed_mm_min;
    float bottom_second_lift_distance_mm;
    float bottom_second_lift_speed_mm_min;
    float second_lift_distance_mm;
    float second_lift_speed_mm_min;
    float bottom_second_retract_distance_mm;
    float bottom_second_retract_speed_mm_min;
    float second_retract_distance_mm;
    float second_retract_speed_mm_min;
    uint16_t bottom_light_pwm;
    uint16_t light_pwm;
    bool advance_mode_layer_definition;
    uint32_t printing_time_s;
    float total_volume_mm3;
    float total_weight_g;
    float total_price;
    char price_unit[8]{};
    uint32_t layer_content_offset;
    bool grayscale_level;
    uint16_t transition_layers;
};
GOO_PACK_POP
static_assert(sizeof(header_info) == 195477, "GOO header is not packed to the V3 layout");

GOO_PACK_PUSH
struct GOO_PACK_ATTR layer_definition
{
    uint8_t reserved{};
    bool pause_at_layer;
    float pause_lift_distance_mm;
    float position_mm;
    float exposure_time_s;
    float off_time_s;
    float before_lift_time_s;
    float after_lift_time_s;
    float after_retract_time_s;
    float lift_distance_mm;
    float lift_speed_mm_min;
    float second_lift_distance_mm;
    float second_lift_speed_mm_min;
    float retract_distance_mm;
    float retract_speed_mm_min;
    float second_retract_distance_mm;
    float second_retract_speed_mm_min;
    uint16_t light_pwm;
    uint8_t delimiter[2] = {0x0d, 0x0a};
};
GOO_PACK_POP
static_assert(sizeof(layer_definition) == 66, "GOO layer definition is not packed");

#undef GOO_PACK_PUSH
#undef GOO_PACK_POP
#undef GOO_PACK_ATTR

static float as_float(const ConfigOption &option)
{
    return float(option.getFloat());
}

static uint16_t as_u16(const ConfigOption &option)
{
    return uint16_t(option.getInt());
}

static void copy_config_string(char *buffer, size_t size, const DynamicPrintConfig &config, const char *key)
{
    if (const auto *option = config.option<ConfigOptionString>(key))
        copy_fixed(buffer, size, option->value);
}

} // namespace

std::unique_ptr<sla::RasterBase> GOOWriter::create_raster() const
{
    const auto ro = m_cfg.display_orientation.getInt();
    auto orientation = ro == sla::RasterBase::roPortrait ? sla::RasterBase::roPortrait
                                                         : sla::RasterBase::roLandscape;

    sla::Resolution resolution{
        size_t(m_cfg.display_pixels_x.getInt()),
        size_t(m_cfg.display_pixels_y.getInt())
    };
    sla::PixelDim pixel_dimensions{
        m_cfg.display_width.getFloat() / resolution.width_px,
        m_cfg.display_height.getFloat() / resolution.height_px
    };
    if (orientation == sla::RasterBase::roPortrait) {
        std::swap(resolution.width_px, resolution.height_px);
        std::swap(pixel_dimensions.w_mm, pixel_dimensions.h_mm);
    }

    std::array<bool, 2> mirror{};
    mirror[X] = m_cfg.display_mirror_x.getBool();
    mirror[Y] = m_cfg.display_mirror_y.getBool();

    return sla::create_raster_grayscale_aa(resolution, pixel_dimensions, m_cfg.gamma_correction.getFloat(),
                                           sla::RasterBase::Trafo{orientation, mirror});
}

sla::RasterEncoder GOOWriter::get_encoder() const
{
    return sla::GOORLERasterEncoder{};
}

void GOOWriter::export_print(const std::string filename, const SLAPrint &print, const ThumbnailsList &thumbnails,
                             const std::string &/*project_name*/)
{
    static constexpr std::array<uint8_t, 11> end_string = {
        0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x44, 0x4c, 0x50, 0x00
    };

    const DynamicPrintConfig &config = print.full_print_config();
    const SLAPrintStatistics &stats = print.print_statistics();
    const SLAPrintObjectConfig &object_cfg = print.default_object_config();
    const SLAMaterialConfig &material = print.material_config();

    const auto ro = m_cfg.display_orientation.getInt();
    const auto orientation = ro == sla::RasterBase::roPortrait ? sla::RasterBase::roPortrait
                                                               : sla::RasterBase::roLandscape;
    sla::Resolution resolution{
        size_t(m_cfg.display_pixels_x.getInt()),
        size_t(m_cfg.display_pixels_y.getInt())
    };
    if (orientation == sla::RasterBase::roPortrait)
        std::swap(resolution.width_px, resolution.height_px);

    const float exposure = as_float(material.exposure_time);
    const float initial_exposure = as_float(material.initial_exposure_time);
    const int bottom_layers = object_cfg.bottom_layers.getInt();
    const int faded_layers = object_cfg.faded_layers.getInt();

    header_info header{};
    copy_fixed(header.software_info, sizeof(header.software_info), SLIC3R_APP_NAME);
    copy_fixed(header.software_version, sizeof(header.software_version), SLIC3R_VERSION);
    copy_fixed(header.file_time, sizeof(header.file_time), Utils::utc_timestamp());
    copy_config_string(header.printer_name, sizeof(header.printer_name), config, "printer_model");
    copy_config_string(header.printer_type, sizeof(header.printer_type), config, "printer_variant");
    copy_config_string(header.resin_profile_name, sizeof(header.resin_profile_name), config, "sla_material_settings_id");

    header.aa_level = hton<uint16_t>(uint16_t(m_cfg.gamma_correction.getFloat() * 100.f));
    header.grey_level = 0;
    header.blur_level = 0;
    header.total_layers = hton<uint32_t>(uint32_t(m_layers.size()));
    header.x_resolution = hton<uint16_t>(uint16_t(resolution.width_px));
    header.y_resolution = hton<uint16_t>(uint16_t(resolution.height_px));
    header.x_mirror = m_cfg.display_mirror_x.getBool() ? 1 : 0;
    header.y_mirror = m_cfg.display_mirror_y.getBool() ? 1 : 0;
    header.x_platform_size_mm = hton(as_float(m_cfg.display_width));
    header.y_platform_size_mm = hton(as_float(m_cfg.display_height));
    header.z_platform_size_mm = hton(as_float(m_cfg.max_print_height));
    header.layer_thickness_mm = hton(as_float(object_cfg.layer_height));
    header.common_exposure_time_s = hton(exposure);
    header.exposure_delivery_time_static = true;
    header.turn_off_time_s = 0;
    header.bottom_before_lift_time_s = hton(as_float(material.sla_initial_wait_before_lift));
    header.bottom_after_lift_time_s = hton(as_float(material.sla_initial_wait_after_lift));
    header.bottom_after_retract_time_s = hton(as_float(material.sla_initial_wait_after_retract));
    header.before_lift_time_s = hton(as_float(material.sla_wait_before_lift));
    header.after_lift_time_s = hton(as_float(material.sla_wait_after_lift));
    header.after_retract_time_s = hton(as_float(material.sla_wait_after_retract));
    header.bottom_exposure_time_s = hton(initial_exposure);
    header.bottom_layers = hton<uint32_t>(uint32_t(std::max(bottom_layers, 0)));
    header.bottom_lift_distance_mm = hton(as_float(material.sla_initial_primary_lift_distance));
    header.bottom_lift_speed_mm_min = hton(as_float(material.sla_initial_primary_lift_speed));
    header.lift_distance_mm = hton(as_float(material.sla_primary_lift_distance));
    header.lift_speed_mm_min = hton(as_float(material.sla_primary_lift_speed));
    header.bottom_retract_distance_mm = hton(as_float(material.sla_initial_primary_retract_distance));
    header.bottom_retract_speed_mm_min = hton(as_float(material.sla_initial_primary_retract_speed));
    header.retract_distance_mm = hton(as_float(material.sla_primary_retract_distance));
    header.retract_speed_mm_min = hton(as_float(material.sla_primary_retract_speed));
    header.bottom_second_lift_distance_mm = hton(as_float(material.sla_initial_secondary_lift_distance));
    header.bottom_second_lift_speed_mm_min = hton(as_float(material.sla_initial_secondary_lift_speed));
    header.second_lift_distance_mm = hton(as_float(material.sla_secondary_lift_distance));
    header.second_lift_speed_mm_min = hton(as_float(material.sla_secondary_lift_speed));
    header.bottom_second_retract_distance_mm = hton(as_float(material.sla_initial_secondary_retract_distance));
    header.bottom_second_retract_speed_mm_min = hton(as_float(material.sla_initial_secondary_retract_speed));
    header.second_retract_distance_mm = hton(as_float(material.sla_secondary_retract_distance));
    header.second_retract_speed_mm_min = hton(as_float(material.sla_secondary_retract_speed));
    header.bottom_light_pwm = hton(as_u16(material.initial_exposure_pwm));
    header.light_pwm = hton(as_u16(material.exposure_pwm));
    // Per-layer definitions carry the motion the Mars firmware actually runs.
    header.advance_mode_layer_definition = true;
    const double print_time = stats.estimated_print_time;
    header.printing_time_s = hton<uint32_t>(std::isfinite(print_time) && print_time > 0. ? uint32_t(print_time) : 0);
    const double density = material.material_density.getFloat();
    const double weight = stats.total_weight;
    header.total_weight_g = hton(std::isfinite(weight) ? float(weight) : 0.f);
    header.total_volume_mm3 = hton(density > 0. && std::isfinite(weight) ? float(weight / density) : 0.f);
    const double bottle_volume = material.bottle_volume.getFloat();
    const double bottle_cost = material.bottle_cost.getFloat();
    const double price = (bottle_volume > 0. && density > 0. && std::isfinite(weight))
        ? bottle_cost / (bottle_volume * density) * weight
        : 0.;
    header.total_price = hton(float(price));
    header.layer_content_offset = hton<uint32_t>(uint32_t(sizeof(header_info)));
    header.grayscale_level = true;
    header.transition_layers = hton<uint16_t>(uint16_t(std::max(faded_layers, 0)));

    write_thumbnail(116, 116, thumbnails, header.small_preview);
    write_thumbnail(290, 290, thumbnails, header.big_preview);

    std::ofstream output(filename, std::ios::binary);
    if (!output)
        throw Slic3r::RuntimeError("Failed to open GOO file for writing: " + filename);
    output.write(reinterpret_cast<const char *>(&header), sizeof(header));

    const float layer_height = as_float(object_cfg.layer_height);
    const float initial_layer_height = as_float(material.initial_layer_height);
    const uint16_t bottom_pwm = as_u16(material.initial_exposure_pwm);
    const uint16_t normal_pwm = as_u16(material.exposure_pwm);

    for (size_t layer_index = 0; layer_index < m_layers.size(); ++layer_index) {
        const int layer_number = int(layer_index) + 1;
        const bool bottom = layer_number <= std::max(bottom_layers, 1);

        layer_definition layer{};
        layer.pause_at_layer = false;
        layer.pause_lift_distance_mm = 0;
        layer.position_mm = hton(initial_layer_height + layer_height * float(layer_index));
        layer.exposure_time_s = hton(layer_exposure_time(layer_number, bottom_layers, faded_layers, initial_exposure, exposure));
        layer.off_time_s = 0;
        layer.before_lift_time_s = hton(as_float(bottom ? material.sla_initial_wait_before_lift : material.sla_wait_before_lift));
        layer.after_lift_time_s = hton(as_float(bottom ? material.sla_initial_wait_after_lift : material.sla_wait_after_lift));
        layer.after_retract_time_s = hton(as_float(bottom ? material.sla_initial_wait_after_retract : material.sla_wait_after_retract));
        layer.lift_distance_mm = hton(as_float(bottom ? material.sla_initial_primary_lift_distance : material.sla_primary_lift_distance));
        layer.lift_speed_mm_min = hton(as_float(bottom ? material.sla_initial_primary_lift_speed : material.sla_primary_lift_speed));
        layer.second_lift_distance_mm = hton(as_float(bottom ? material.sla_initial_secondary_lift_distance : material.sla_secondary_lift_distance));
        layer.second_lift_speed_mm_min = hton(as_float(bottom ? material.sla_initial_secondary_lift_speed : material.sla_secondary_lift_speed));
        layer.retract_distance_mm = hton(as_float(bottom ? material.sla_initial_primary_retract_distance : material.sla_primary_retract_distance));
        layer.retract_speed_mm_min = hton(as_float(bottom ? material.sla_initial_primary_retract_speed : material.sla_primary_retract_speed));
        layer.second_retract_distance_mm = hton(as_float(bottom ? material.sla_initial_secondary_retract_distance : material.sla_secondary_retract_distance));
        layer.second_retract_speed_mm_min = hton(as_float(bottom ? material.sla_initial_secondary_retract_speed : material.sla_secondary_retract_speed));
        layer.light_pwm = hton(bottom ? bottom_pwm : normal_pwm);

        output.write(reinterpret_cast<const char *>(&layer), sizeof(layer));
        output.write(reinterpret_cast<const char *>(m_layers[layer_index].data()), std::streamsize(m_layers[layer_index].size()));
    }

    output.write(reinterpret_cast<const char *>(end_string.data()), std::streamsize(end_string.size()));
    if (!output)
        throw Slic3r::RuntimeError("Failed while writing GOO file: " + filename);
}

} // namespace Slic3r
