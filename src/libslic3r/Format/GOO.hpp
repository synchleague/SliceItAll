///|/ Copyright (c) 2024 Felix Reißmann @Felix-Rm
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/

#pragma once

#include "SLAArchiveWriter.hpp"

#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {

// Writes an Elegoo GOO V3 archive. Import is intentionally absent: the archive
// test skips formats that have no reader, and reconstructing a mesh from RLE
// layers is a separate task.
class GOOWriter : public SLAArchiveWriter
{
    SLAPrinterConfig m_cfg{};

protected:
    std::unique_ptr<sla::RasterBase> create_raster() const override;
    sla::RasterEncoder get_encoder() const override;

public:
    GOOWriter() = default;
    explicit GOOWriter(const SLAPrinterConfig &cfg) : m_cfg(cfg) {}
    explicit GOOWriter(SLAPrinterConfig &&cfg) : m_cfg(std::move(cfg)) {}

    void export_print(const std::string     filename,
                      const SLAPrint       &print,
                      const ThumbnailsList &thumbnails,
                      const std::string    &project_name = "") override;
};

} // namespace Slic3r
