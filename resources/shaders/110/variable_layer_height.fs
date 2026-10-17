#version 110

#define M_PI 3.1415926535897932384626433832795

// 2D texture (1D texture split by the rows) of color along the object Z axis.
uniform sampler2D z_texture;
// Scaling from the Z texture rows coordinate to the normalized texture row coordinate.
uniform float z_to_texture_row;
uniform float z_texture_row_to_normalized;
uniform float z_cursor;
uniform float z_cursor_band_width;

// x = tainted, y = specular;
varying vec2 intensity;

varying float object_z;

void main()
{
    float object_z_row = z_to_texture_row * object_z;
    // Index of the row in the texture.
    float z_texture_row = floor(object_z_row);
    // Normalized coordinate from 0. to 1.
    float z_texture_col = object_z_row - z_texture_row;
    float z_blend = 0.25 * cos(min(M_PI, abs(M_PI * (object_z - z_cursor) * 1.8 / z_cursor_band_width))) + 0.25;
    // Sample the Z texture. Texture coordinates are normalized to <0, 1>.
    // GLSL 110 has no fragment texture2DLod. Sample the detailed mip without a bias;
    // a huge bias is ignored by some drivers and the color comes back black.
    vec4 color = vec4(0.25, 0.25, 0.25, 1.0);
    if (z_texture_row >= 0.0)
        color = texture2D(z_texture, vec2(z_texture_col, z_texture_row_to_normalized * (z_texture_row + 0.5)));            
    // Mix the final color.
    gl_FragColor = vec4(vec3(intensity.y), 1.0) +  intensity.x * mix(color, vec4(1.0, 1.0, 0.0, 1.0), z_blend);
}
