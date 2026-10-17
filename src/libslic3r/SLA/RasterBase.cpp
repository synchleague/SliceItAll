///|/ Copyright (c) Prusa Research 2020 - 2022 Tomáš Mészáros @tamasmeszaros
///|/ Copyright (c) 2022 ole00 @ole00
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef SLARASTER_CPP
#define SLARASTER_CPP

#include <libslic3r/SLA/RasterBase.hpp>
#include <libslic3r/SLA/AGGRaster.hpp>
// minz image write:
#include <miniz.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <cstdlib>
#include <stdexcept>

#include "agg/agg_gamma_functions.h"

namespace Slic3r { namespace sla {

EncodedRaster PNGRasterEncoder::operator()(const void *ptr, size_t w, size_t h,
                                           size_t      num_components)
{
    std::vector<uint8_t> buf;
    size_t s = 0;
    
    void *rawdata = tdefl_write_image_to_png_file_in_memory(
        ptr, int(w), int(h), int(num_components), &s);
    
    // On error, data() will return an empty vector. No other info can be
    // retrieved from miniz anyway...
    if (rawdata == nullptr) return EncodedRaster({}, "png");
    
    auto pptr = static_cast<std::uint8_t*>(rawdata);
    
    buf.reserve(s);
    std::copy(pptr, pptr + s, std::back_inserter(buf));
    
    MZ_FREE(rawdata);
    return EncodedRaster(std::move(buf), "png");
}

std::ostream &operator<<(std::ostream &stream, const EncodedRaster &bytes)
{
    stream.write(reinterpret_cast<const char *>(bytes.data()),
                 std::streamsize(bytes.size()));
    
    return stream;
}

EncodedRaster PPMRasterEncoder::operator()(const void *ptr, size_t w, size_t h,
                                           size_t      num_components)
{
    std::vector<uint8_t> buf;
    
    auto header = std::string("P5 ") +
            std::to_string(w) + " " +
            std::to_string(h) + " " + "255 ";
    
    auto sz = w * h * num_components;
    size_t s = sz + header.size();
    
    buf.reserve(s);

    auto buff = reinterpret_cast<const std::uint8_t*>(ptr);
    std::copy(header.begin(), header.end(), std::back_inserter(buf));
    std::copy(buff, buff+sz, std::back_inserter(buf));
    
    return EncodedRaster(std::move(buf), "ppm");
}

EncodedRaster GOORLERasterEncoder::operator()(const void *ptr, size_t w, size_t h, size_t num_components)
{
    // GOO layer body: 4-byte big-endian payload length, magic 0x55, RLE bytes,
    // then an inverted 8-bit sum of everything after the length, then CRLF.
    static constexpr uint8_t magic = 0x55;
    static constexpr uint8_t header_size = 5;

    if (ptr == nullptr)
        throw std::runtime_error("GOO encoder received an empty image");
    if (num_components != 1)
        throw std::runtime_error("GOO encoder requires a grayscale image");

    const auto *pixels = static_cast<const uint8_t *>(ptr);
    const size_t pixel_count = w * h;

    std::vector<uint8_t> output(header_size);
    output[4] = magic;

    size_t index = 0;
    while (index < pixel_count) {
        const uint8_t value = pixels[index];
        // A run stops one pixel before the end of the buffer, matching the encoder
        // that was tested on a Mars 4 Max. The leftover pixel becomes its own run.
        size_t run_length = 1;
        while (run_length + 1 < pixel_count - index && run_length < 0x0fffffffu) {
            if (pixels[index + run_length - 1] == pixels[index + run_length])
                ++run_length;
            else
                break;
        }

        const uint8_t type_tag = value == 0 ? 0 : value == 255 ? 3 : 1;
        const uint8_t length_tag = run_length < 16 ? 0 : run_length < 4096 ? 1 : run_length < 1048576 ? 2 : 3;
        // Low nibble is the low 4 length bits. Matches the bitfield order used by the
        // Mars 4 Max GOO writer this encoder was ported from.
        output.push_back(static_cast<uint8_t>((type_tag << 6) | (length_tag << 4) | (run_length & 0x0f)));
        if (type_tag == 1)
            output.push_back(value);
        if (length_tag == 3)
            output.push_back(static_cast<uint8_t>((run_length >> 20) & 0xff));
        if (length_tag >= 2)
            output.push_back(static_cast<uint8_t>((run_length >> 12) & 0xff));
        if (length_tag >= 1)
            output.push_back(static_cast<uint8_t>((run_length >> 4) & 0xff));

        index += run_length;
    }

    // Length covers the magic byte through the checksum, excluding the length field itself.
    const uint32_t encoded_length = static_cast<uint32_t>(output.size() - header_size + 2);
    output[0] = static_cast<uint8_t>(encoded_length >> 24);
    output[1] = static_cast<uint8_t>((encoded_length >> 16) & 0xff);
    output[2] = static_cast<uint8_t>((encoded_length >> 8) & 0xff);
    output[3] = static_cast<uint8_t>(encoded_length & 0xff);

    uint8_t checksum = 0;
    for (size_t i = 5; i < output.size(); ++i)
        checksum = static_cast<uint8_t>(checksum + output[i]);
    output.push_back(static_cast<uint8_t>(~checksum));
    output.push_back(0x0d);
    output.push_back(0x0a);

    return EncodedRaster(std::move(output), "");
}

std::unique_ptr<RasterBase> create_raster_grayscale_aa(
    const Resolution        &res,
    const PixelDim          &pxdim,
    double                   gamma,
    const RasterBase::Trafo &tr)
{
    std::unique_ptr<RasterBase> rst;
    
    if (gamma > 0)
        rst = std::make_unique<RasterGrayscaleAAGammaPower>(res, pxdim, tr, gamma);
    else if (std::abs(gamma - 1.) < 1e-6)
        rst = std::make_unique<RasterGrayscaleAA>(res, pxdim, tr, agg::gamma_none());
    else
        rst = std::make_unique<RasterGrayscaleAA>(res, pxdim, tr, agg::gamma_threshold(.5));
    
    return rst;
}

} // namespace sla
} // namespace Slic3r

#endif // SLARASTER_CPP
