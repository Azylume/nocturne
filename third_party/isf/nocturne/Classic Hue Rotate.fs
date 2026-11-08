/*{
    "CREDIT": "Nocturne, AGPL-3.0-or-later",
    "DESCRIPTION": "In HSL, the hue times hueScale plus hueRotate in turns, the saturation times 2 x satScale",
    "NOCTURNE": {
        "TYPE": "classicHueRotate",
        "CATEGORY": "Classic",
        "STAGE_OPACITY": true,
        "PARAMS": {
            "hueRotate": {"WRAPS": true},
            "blendMode": {"UNIT": {"SUFFIX": "", "SCALE": 1, "DECIMALS": 0}}
        }
    },
    "INPUTS": [
        {"NAME": "inputImage", "TYPE": "image"},
        {"NAME": "hueScale", "LABEL": "Hue Scale", "TYPE": "float", "MIN": 0, "MAX": 1, "DEFAULT": 1},
        {"NAME": "hueRotate", "LABEL": "Hue Rotate", "TYPE": "float", "MIN": 0, "MAX": 1, "DEFAULT": 0},
        {"NAME": "satScale", "LABEL": "Sat. Scale", "TYPE": "float", "MIN": 0, "MAX": 1, "DEFAULT": 0.5},
        {"NAME": "blendMode", "LABEL": "Blend Mode", "TYPE": "long", "DEFAULT": 0, "VALUES": [0, 1], "LABELS": ["Alpha", "Add"]}
    ],
    "ISFVSN": "2"
}
*/

// The Add blend mode crossfades additively; the stage then mixes by Opacity, so this undoes that mix.
vec4 blended(vec4 c, vec4 o)
{
    if (blendMode == 0)
        return o;
    float w = max(STAGE_OPACITY, 1e-4);
    vec3 faded = c.rgb * min(1.0, 2.0 - 2.0 * w) + o.rgb * min(1.0, 2.0 * w);
    return vec4(c.rgb + (faded - c.rgb) / w, o.a);
}

void main()
{
    vec4 c = IMG_THIS_PIXEL(inputImage);
    float hi = max(c.r, max(c.g, c.b)), lo = min(c.r, min(c.g, c.b)), d = hi - lo;
    float l = (hi + lo) * 0.5;
    float s = d / max(1.0 - abs(2.0 * l - 1.0), 1e-5);
    float hue = d <= 0.0 ? 0.0 : hi == c.r ? (c.g - c.b) / d : hi == c.g ? 2.0 + (c.b - c.r) / d : 4.0 + (c.r - c.g) / d;
    hue = fract(hue / 6.0 * hueScale + hueRotate);
    s = clamp(s * 2.0 * satScale, 0.0, 1.0);
    vec3 pure = clamp(abs(fract(hue + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0);
    vec4 o = vec4(clamp(l + (pure - 0.5) * s * (1.0 - abs(2.0 * l - 1.0)), 0.0, 1.0), c.a);
    gl_FragColor = blended(c, o);
}
