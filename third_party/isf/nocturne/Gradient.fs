/*{
    "CREDIT": "Nocturne, AGPL-3.0-or-later",
    "DESCRIPTION": "From the first colour to the second across the picture along angle, scale times over (mirrored), scrolling at speed ramps per second",
    "NOCTURNE": {
        "TYPE": "gradient",
        "CATEGORY": "",
        "PARAMS": {
            "angle": {"WRAPS": true, "UNIT": {"SUFFIX": "°", "SCALE": 1, "DECIMALS": 0}},
            "scale": {"UNIT": {"SUFFIX": "x", "SCALE": 1, "DECIMALS": 2}},
            "speed": {"DRIVE": "PHASE", "UNIT": {"SUFFIX": "/s", "SCALE": 1, "DECIMALS": 2}},
            "red": {"UNIT": {"SUFFIX": "%", "SCALE": 100, "DECIMALS": 0}},
            "green": {"UNIT": {"SUFFIX": "%", "SCALE": 100, "DECIMALS": 0}},
            "blue": {"UNIT": {"SUFFIX": "%", "SCALE": 100, "DECIMALS": 0}},
            "toRed": {"UNIT": {"SUFFIX": "%", "SCALE": 100, "DECIMALS": 0}},
            "toGreen": {"UNIT": {"SUFFIX": "%", "SCALE": 100, "DECIMALS": 0}},
            "toBlue": {"UNIT": {"SUFFIX": "%", "SCALE": 100, "DECIMALS": 0}}
        }
    },
    "INPUTS": [
        {"NAME": "angle", "TYPE": "float", "MIN": -180, "MAX": 180, "DEFAULT": 0},
        {"NAME": "scale", "TYPE": "float", "MIN": 0.25, "MAX": 8, "DEFAULT": 1},
        {"NAME": "speed", "TYPE": "float", "MIN": -4, "MAX": 4, "DEFAULT": 0},
        {"NAME": "red", "TYPE": "float", "MIN": 0, "MAX": 1, "DEFAULT": 0},
        {"NAME": "green", "TYPE": "float", "MIN": 0, "MAX": 1, "DEFAULT": 0},
        {"NAME": "blue", "TYPE": "float", "MIN": 0, "MAX": 1, "DEFAULT": 0},
        {"NAME": "toRed", "TYPE": "float", "MIN": 0, "MAX": 1, "DEFAULT": 1},
        {"NAME": "toGreen", "TYPE": "float", "MIN": 0, "MAX": 1, "DEFAULT": 1},
        {"NAME": "toBlue", "TYPE": "float", "MIN": 0, "MAX": 1, "DEFAULT": 1}
    ],
    "ISFVSN": "2"
}
*/

void main()
{
    vec2 px = vec2(gl_FragCoord.x, RENDERSIZE.y - gl_FragCoord.y);
    float a = angle * (3.14159265358979 / 180.0);
    vec2 d = vec2(cos(a), sin(a));
    float t = dot(px - RENDERSIZE * 0.5, d) / (abs(d.x) * RENDERSIZE.x + abs(d.y) * RENDERSIZE.y);
    t = t * scale + 0.5 - speed;
    t = 1.0 - abs(1.0 - 2.0 * fract(t * 0.5));
    gl_FragColor = vec4(mix(vec3(red, green, blue), vec3(toRed, toGreen, toBlue), t), 1.0);
}
