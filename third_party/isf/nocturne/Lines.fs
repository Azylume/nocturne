/*{
    "CREDIT": "Nocturne, AGPL-3.0-or-later",
    "DESCRIPTION": "Parallel lines across the height, each width of its period wide, moving at speed lines per second; edges are box filtered over a pixel",
    "NOCTURNE": {
        "TYPE": "lines",
        "CATEGORY": "",
        "PARAMS": {
            "count": {"UNIT": {"SUFFIX": " lines", "SCALE": 1, "DECIMALS": 1}},
            "width": {"UNIT": {"SUFFIX": "%", "SCALE": 100, "DECIMALS": 0}},
            "angle": {"WRAPS": true, "UNIT": {"SUFFIX": "°", "SCALE": 1, "DECIMALS": 0}},
            "speed": {"DRIVE": "PHASE", "UNIT": {"SUFFIX": " lines/s", "SCALE": 1, "DECIMALS": 2}},
            "softness": {"UNIT": {"SUFFIX": "%", "SCALE": 100, "DECIMALS": 0}},
            "red": {"UNIT": {"SUFFIX": "%", "SCALE": 100, "DECIMALS": 0}},
            "green": {"UNIT": {"SUFFIX": "%", "SCALE": 100, "DECIMALS": 0}},
            "blue": {"UNIT": {"SUFFIX": "%", "SCALE": 100, "DECIMALS": 0}}
        }
    },
    "INPUTS": [
        {"NAME": "count", "TYPE": "float", "MIN": 1, "MAX": 64, "DEFAULT": 8},
        {"NAME": "width", "TYPE": "float", "MIN": 0, "MAX": 1, "DEFAULT": 0.5},
        {"NAME": "angle", "TYPE": "float", "MIN": -180, "MAX": 180, "DEFAULT": 0},
        {"NAME": "speed", "TYPE": "float", "MIN": -8, "MAX": 8, "DEFAULT": 0},
        {"NAME": "softness", "TYPE": "float", "MIN": 0, "MAX": 1, "DEFAULT": 0},
        {"NAME": "red", "TYPE": "float", "MIN": 0, "MAX": 1, "DEFAULT": 1},
        {"NAME": "green", "TYPE": "float", "MIN": 0, "MAX": 1, "DEFAULT": 1},
        {"NAME": "blue", "TYPE": "float", "MIN": 0, "MAX": 1, "DEFAULT": 1}
    ],
    "ISFVSN": "2"
}
*/

void main()
{
    vec2 px = vec2(gl_FragCoord.x, RENDERSIZE.y - gl_FragCoord.y);
    float a = angle * (3.14159265358979 / 180.0);
    vec2 d = vec2(cos(a), sin(a));
    float perPixel = count / RENDERSIZE.y;
    float s = dot(px - RENDERSIZE * 0.5, d) * perPixel - speed;
    float v = abs(fract(s + 0.5) - 0.5);
    float hw = width * 0.5;
    float w = max(perPixel, softness * 0.5);
    float cover = max(min(v + w * 0.5, hw) - max(v - w * 0.5, -hw), 0.0) +
                  max(min(v + w * 0.5, 1.0 + hw) - max(v - w * 0.5, 1.0 - hw), 0.0);
    gl_FragColor = vec4(red, green, blue, clamp(cover / w, 0.0, 1.0));
}
