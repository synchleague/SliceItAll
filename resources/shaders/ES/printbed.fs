#version 100

precision highp float;

const vec3 back_color_dark  = vec3(0.145, 0.145, 0.165);
const vec3 back_color_light = vec3(0.220, 0.220, 0.245);

uniform sampler2D texture;
uniform bool transparent_background;
uniform bool svg_source;

varying vec2 tex_coord;

vec4 svg_color()
{
    // takes foreground from texture
    vec4 fore_color = texture2D(texture, tex_coord);

    // calculates radial gradient
    vec3 back_color = vec3(mix(back_color_light, back_color_dark, smoothstep(0.0, 0.5, length(abs(tex_coord.xy) - vec2(0.5)))));

    // blends foreground with background
    return vec4(mix(back_color, fore_color.rgb, fore_color.a), transparent_background ? fore_color.a : 1.0);
}

vec4 non_svg_color()
{
    // takes foreground from texture
    vec4 color = texture2D(texture, tex_coord);
    // Pull bright bed photos down into the dark plate without flattening the pattern.
    color.rgb *= vec3(0.48, 0.48, 0.52);
    return vec4(color.rgb, transparent_background ? color.a * 0.25 : color.a);
}

void main()
{
	vec4 color = svg_source ? svg_color() : non_svg_color();
	color.a = transparent_background ? color.a * 0.5 : color.a;
	gl_FragColor = color;
}