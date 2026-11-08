/*{
    "CREDIT": "Nocturne, AGPL-3.0-or-later",
    "DESCRIPTION": "Squares across the height, turned by angle, scrolling at speed squares per second; the others are transparent",
    "NOCTURNE": {
        "TYPE": "checker",
        "CATEGORY": "",
        "PARAMS": {
            "count": {"UNIT": {"SUFFIX": " cells", "SCALE": 1, "DECIMALS": 1}},
            "angle": {"WRAPS": true, "UNIT": {"SUFFIX": "°", "SCALE": 1, "DECIMALS": 0}},
            "speed": {"DRIVE": "PHASE", "UNIT": {"SUFFIX": " cells/s", "SCALE": 1, "DECIMALS": 2}},
            "red": {"UNIT": {"SUFFIX": "%", "SCALE": 100, "DECIMALS": 0}},
            "green": {"UNIT": {"SUFFIX": "%", "SCALE": 100, "DECIMALS": 0}},
            "blue": {"UNIT": {"SUFFIX": "%", "SCALE": 100, "DECIMALS": 0}}
        }
    },
    "INPUTS": [
        {"NAME": "count", "TYPE": "float", "MIN": 1, "MAX": 64, "DEFAULT": 8},
        {"NAME": "angle", "TYPE": "float", "MIN": -180, "MAX": 180, "DEFAULT": 0},
        {"NAME": "speed", "TYPE": "float", "MIN": -8, "MAX": 8, "DEFAULT": 0},
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
    float s = sin(a), k = cos(a);
    float perPixel = count / RENDERSIZE.y;
    vec2 q = px - RENDERSIZE * 0.5;
    q = vec2(k * q.x + s * q.y, -s * q.x + k * q.y) * perPixel;
    q.x -= speed;
    vec2 lo = q - 0.5 * perPixel, hi = q + 0.5 * perPixel;
    vec2 sq = (abs(fract(lo) - 0.5) - abs(fract(hi) - 0.5)) / perPixel;
    gl_FragColor = vec4(red, green, blue, clamp(0.5 + 0.5 * sq.x * sq.y, 0.0, 1.0));
}
