/*{
    "CREDIT": "Nocturne, AGPL-3.0-or-later",
    "DESCRIPTION": "Replaces the colour by luminance (inverted on request) times a tint of the given hue, saturation and brightness; alpha is how much of the tinted picture replaces the original",
    "NOCTURNE": {
        "TYPE": "colorize",
        "CATEGORY": "",
        "PARAMS": {
            "hue": {"UNIT": {"SUFFIX": "°", "SCALE": 360, "DECIMALS": 0}, "WRAPS": true},
            "saturation": {"UNIT": {"SUFFIX": "%", "SCALE": 100, "DECIMALS": 0}},
            "brightness": {"UNIT": {"SUFFIX": "%", "SCALE": 100, "DECIMALS": 0}},
            "invert": {"UNIT": {"SUFFIX": "%", "SCALE": 100, "DECIMALS": 0}},
            "alpha": {"UNIT": {"SUFFIX": "%", "SCALE": 100, "DECIMALS": 0}}
        }
    },
    "INPUTS": [
        {"NAME": "inputImage", "TYPE": "image"},
        {"NAME": "hue", "TYPE": "float", "MIN": 0, "MAX": 1, "DEFAULT": 0},
        {"NAME": "saturation", "TYPE": "float", "MIN": 0, "MAX": 1, "DEFAULT": 1},
        {"NAME": "brightness", "TYPE": "float", "MIN": 0, "MAX": 1, "DEFAULT": 1},
        {"NAME": "invert", "TYPE": "float", "MIN": 0, "MAX": 1, "DEFAULT": 0},
        {"NAME": "alpha", "TYPE": "float", "MIN": 0, "MAX": 1, "DEFAULT": 1}
    ],
    "ISFVSN": "2"
}
*/

void main()
{
    vec4 c = IMG_THIS_PIXEL(inputImage);
    if (alpha == 0.0) {
        gl_FragColor = c;
        return;
    }
    vec3 tint = clamp(abs(fract(hue + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0);
    tint = mix(vec3(1.0), tint, saturation) * brightness;
    float y = dot(c.rgb, vec3(0.2126, 0.7152, 0.0722));
    gl_FragColor = vec4(mix(c.rgb, mix(y, 1.0 - y, invert) * tint, alpha), c.a);
}
