///|/ Slice it All
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_GLGizmoBrimEars_hpp_
#define slic3r_GLGizmoBrimEars_hpp_

#include "GLGizmoBase.hpp"

namespace Slic3r {
namespace GUI {

class GLGizmoBrimEars : public GLGizmoBase
{
public:
    GLGizmoBrimEars(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id);

protected:
    bool on_init() override;
    std::string on_get_name() const override;
    void on_render() override;
    void on_render_input_window(float x, float y, float bottom_limit) override;
    bool on_is_activable() const override;
    bool on_is_selectable() const override { return true; }
    CommonGizmosDataID on_get_requirements() const override;

private:
    void add_ear();

    float m_diameter{ 12.f };
    float m_thickness{ 0.2f };
    int   m_next_corner{ 0 };
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GLGizmoBrimEars_hpp_
