/*{
    "CREDIT": "Nocturne, AGPL-3.0-or-later",
    "DESCRIPTION": "Colour 1 where the luma is above the threshold, colour 2 below, blended by a smoothstep of the given smoothness; alpha keeps the picture's, combines with it or is 1",
    "NOCTURNE": {
        "TYPE": "classicThreshold",
        "CATEGORY": "Classic",
        "STAGE_OPACITY": true,
        "PARAMS": {
            "alpha": {"UNIT": {"SUFFIX": "", "SCALE": 1, "DECIMALS": 0}},
            "blendMode": {"UNIT": {"SUFFIX": "", "SCALE": 1, "DECIMALS": 0}}
        }
    },
    "INPUTS": [
        {"NAME": "inputImage", "TYPE": "image"},
        {"NAME": "threshold", "LABEL": "Threshold", "TYPE": "float", "MIN": 0, "MAX": 1, "DEFAULT": 0.5},
        {"NAME": "smoothness", "LABEL": "Smoothness", "TYPE": "float", "MIN": 0, "MAX": 1, "DEFAULT": 0},
        {"NAME": "color1Red", "LABEL": "Color 1 Red", "TYPE": "float", "MIN": 0, "MAX": 1, "DEFAULT": 1},
        {"NAME": "color1Green", "LABEL": "Color 1 Green", "TYPE": "float", "MIN": 0, "MAX": 1, "DEFAULT": 1},
        {"NAME": "color1Blue", "LABEL": "Color 1 Blue", "TYPE": "float", "MIN": 0, "MAX": 1, "DEFAULT": 1},
        {"NAME": "color1Alpha", "LABEL": "Color 1 Alpha", "TYPE": "float", "MIN": 0, "MAX": 1, "DEFAULT": 1},
        {"NAME": "color2Red", "LABEL": "Color 2 Red", "TYPE": "float", "MIN": 0, "MAX": 1, "DEFAULT": 0},
        {"NAME": "color2Green", "LABEL": "Color 2 Green", "TYPE": "float", "MIN": 0, "MAX": 1, "DEFAULT": 0},
        {"NAME": "color2Blue", "LABEL": "Color 2 Blue", "TYPE": "float", "MIN": 0, "MAX": 1, "DEFAULT": 0},
        {"NAME": "color2Alpha", "LABEL": "Color 2 Alpha", "TYPE": "float", "MIN": 0, "MAX": 1, "DEFAULT": 1},
        {"NAME": "alpha", "LABEL": "Alpha", "TYPE": "long", "DEFAULT": 0, "VALUES": [0, 1, 2], "LABELS": ["Original", "Combine", "Replace"]},
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
    float y = dot(c.rgb, vec3(0.30, 0.59, 0.11));
    float soft = smoothness * 0.5;
    float below = soft > 0.0 ? smoothstep(max(y - soft, 0.0), min(y + soft, 1.0), threshold) : float(y <= threshold);
    vec4 one = vec4(color1Red, color1Green, color1Blue, color1Alpha);
    vec4 two = vec4(color2Red, color2Green, color2Blue, color2Alpha);
    vec4 o = mix(vec4(one.rgb * one.a, one.a), vec4(two.rgb * two.a, two.a), below);
    o.a = alpha == 0 ? c.a : alpha == 1 ? o.a * c.a : 1.0;
    gl_FragColor = blended(c, o);
}
