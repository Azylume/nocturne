/*{
    "CREDIT": "Nocturne, AGPL-3.0-or-later",
    "DESCRIPTION": "Concentric rings from the centre to the top edge, each width of its period wide, around a centre moved by x and y, spreading at speed rings per second; edges are box filtered over a pixel",
    "NOCTURNE": {
        "TYPE": "rings",
        "CATEGORY": "",
        "PARAMS": {
            "count": {"UNIT": {"SUFFIX": " rings", "SCALE": 1, "DECIMALS": 1}},
            "width": {"UNIT": {"SUFFIX": "%", "SCALE": 100, "DECIMALS": 0}},
            "speed": {"DRIVE": "PHASE", "UNIT": {"SUFFIX": " rings/s", "SCALE": 1, "DECIMALS": 2}},
            "softness": {"UNIT": {"SUFFIX": "%", "SCALE": 100, "DECIMALS": 0}},
            "x": {"UNIT": {"SUFFIX": "%", "SCALE": 100, "DECIMALS": 0}},
            "y": {"UNIT": {"SUFFIX": "%", "SCALE": 100, "DECIMALS": 0}},
            "red": {"UNIT": {"SUFFIX": "%", "SCALE": 100, "DECIMALS": 0}},
            "green": {"UNIT": {"SUFFIX": "%", "SCALE": 100, "DECIMALS": 0}},
            "blue": {"UNIT": {"SUFFIX": "%", "SCALE": 100, "DECIMALS": 0}}
        }
    },
    "INPUTS": [
        {"NAME": "count", "TYPE": "float", "MIN": 1, "MAX": 64, "DEFAULT": 8},
        {"NAME": "width", "TYPE": "float", "MIN": 0, "MAX": 1, "DEFAULT": 0.5},
        {"NAME": "speed", "TYPE": "float", "MIN": -8, "MAX": 8, "DEFAULT": 0},
        {"NAME": "softness", "TYPE": "float", "MIN": 0, "MAX": 1, "DEFAULT": 0},
        {"NAME": "x", "TYPE": "float", "MIN": -0.5, "MAX": 0.5, "DEFAULT": 0},
        {"NAME": "y", "TYPE": "float", "MIN": -0.5, "MAX": 0.5, "DEFAULT": 0},
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
    float perPixel = count / (0.5 * RENDERSIZE.y);
    vec2 centre = RENDERSIZE * (0.5 + vec2(x, -y));
    float s = length(px - centre) * perPixel - speed;
    float v = abs(fract(s + 0.5) - 0.5);
    float hw = width * 0.5;
    float w = max(perPixel, softness * 0.5);
    float cover = max(min(v + w * 0.5, hw) - max(v - w * 0.5, -hw), 0.0) +
                  max(min(v + w * 0.5, 1.0 + hw) - max(v - w * 0.5, 1.0 - hw), 0.0);
    gl_FragColor = vec4(red, green, blue, clamp(cover / w, 0.0, 1.0));
}
