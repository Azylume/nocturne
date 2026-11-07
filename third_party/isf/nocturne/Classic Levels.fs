/*{
    "CREDIT": "Nocturne, AGPL-3.0-or-later",
    "DESCRIPTION": "Colour^(2 x (1 - midpoint)), times (2 x ratio)^2, plus 4 x (base - 0.5); all three neutral at 0.5",
    "NOCTURNE": {
        "TYPE": "classicLevels",
        "CATEGORY": "Classic",
        "STAGE_OPACITY": true,
        "PARAMS": {
            "blendMode": {"UNIT": {"SUFFIX": "", "SCALE": 1, "DECIMALS": 0}}
        }
    },
    "INPUTS": [
        {"NAME": "inputImage", "TYPE": "image"},
        {"NAME": "base", "LABEL": "Base", "TYPE": "float", "MIN": 0, "MAX": 1, "DEFAULT": 0.5},
        {"NAME": "midpoint", "LABEL": "Midpoint", "TYPE": "float", "MIN": 0, "MAX": 1, "DEFAULT": 0.5},
        {"NAME": "ratio", "LABEL": "Ratio", "TYPE": "float", "MIN": 0, "MAX": 1, "DEFAULT": 0.5},
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
    vec3 x = mix(pow(c.rgb, vec3(2.0 * (1.0 - midpoint))), vec3(0.0), lessThanEqual(c.rgb, vec3(0.0)));
    gl_FragColor = blended(c, vec4(clamp(x * (4.0 * ratio * ratio) + 4.0 * (base - 0.5), 0.0, 1.0), c.a));
}
