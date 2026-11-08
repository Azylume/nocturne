/*{
    "CREDIT": "Nocturne, AGPL-3.0-or-later",
    "DESCRIPTION": "A flash every 2 x (1 - frequency) seconds, or of that many beats of the beat clock at 120 BPM, on for half of it or as a ramp (Fade), replacing the picture with the colour at its alpha",
    "NOCTURNE": {
        "TYPE": "classicStrobe",
        "CATEGORY": "Classic",
        "STAGE_OPACITY": true,
        "BEAT_CLOCK": true,
        "PARAMS": {
            "fade": {"UNIT": {"SUFFIX": "", "SCALE": 1, "DECIMALS": 0}},
            "syncToBeat": {"UNIT": {"SUFFIX": "", "SCALE": 1, "DECIMALS": 0}},
            "blendMode": {"UNIT": {"SUFFIX": "", "SCALE": 1, "DECIMALS": 0}}
        }
    },
    "INPUTS": [
        {"NAME": "inputImage", "TYPE": "image"},
        {"NAME": "frequency", "LABEL": "Frequency", "TYPE": "float", "MIN": 0, "MAX": 1, "DEFAULT": 0.5},
        {"NAME": "fade", "LABEL": "Fade", "TYPE": "long", "DEFAULT": 0, "VALUES": [0, 1], "LABELS": ["Off", "On"]},
        {"NAME": "colorRed", "LABEL": "Color Red", "TYPE": "float", "MIN": 0, "MAX": 1, "DEFAULT": 1},
        {"NAME": "colorGreen", "LABEL": "Color Green", "TYPE": "float", "MIN": 0, "MAX": 1, "DEFAULT": 1},
        {"NAME": "colorBlue", "LABEL": "Color Blue", "TYPE": "float", "MIN": 0, "MAX": 1, "DEFAULT": 1},
        {"NAME": "colorAlpha", "LABEL": "Color Alpha", "TYPE": "float", "MIN": 0, "MAX": 1, "DEFAULT": 1},
        {"NAME": "syncToBeat", "LABEL": "Sync to Beat", "TYPE": "long", "DEFAULT": 1, "VALUES": [0, 1], "LABELS": ["Off", "On"]},
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
    float period = 2.0 * (1.0 - frequency);
    float clock = syncToBeat == 1 ? BEAT * 0.5 : TIME;
    float ramp = period > 0.0 ? 1.0 - abs(2.0 * fract(clock / period) - 1.0) : 1.0;
    float flash = frequency <= 0.0 ? 0.0 : fade == 1 ? ramp : step(0.5, ramp);
    // On its nearest 8-bit step, a half rounding up: a GPU may store a colour of exactly half a step either way.
    vec3 colour = floor(vec3(colorRed, colorGreen, colorBlue) * 255.0 + 0.5) / 255.0;
    gl_FragColor = blended(c, mix(c, vec4(colour, colorAlpha), flash));
}
