/*{
    "CREDIT": "Nocturne, AGPL-3.0-or-later",
    "DESCRIPTION": "Adds brightness to the colour, then scales it about black by 1 + contrast below 0 and 1 + 5 x contrast above",
    "NOCTURNE": {
        "TYPE": "classicBrightnessContrast",
        "CATEGORY": "Classic",
        "STAGE_OPACITY": true,
        "PARAMS": {
            "brightness": {"UNIT": {"SUFFIX": " %", "SCALE": 100, "DECIMALS": 0}},
            "blendMode": {"UNIT": {"SUFFIX": "", "SCALE": 1, "DECIMALS": 0}}
        }
    },
    "INPUTS": [
        {"NAME": "inputImage", "TYPE": "image"},
        {"NAME": "brightness", "LABEL": "Brightness", "TYPE": "float", "MIN": -1, "MAX": 1, "DEFAULT": 0},
        {"NAME": "contrast", "LABEL": "Contrast", "TYPE": "float", "MIN": -1, "MAX": 1, "DEFAULT": 0},
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
    float gain = 1.0 + (contrast < 0.0 ? contrast : 5.0 * contrast);
    gl_FragColor = blended(c, vec4(clamp((c.rgb + brightness) * gain, 0.0, 1.0), c.a));
}
