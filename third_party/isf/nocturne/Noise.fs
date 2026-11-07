/*{
    "CREDIT": "Nocturne, AGPL-3.0-or-later",
    "DESCRIPTION": "Smooth value noise, scale blobs across the height, evolving at speed per second; contrast pushes it towards black and the colour",
    "NOCTURNE": {
        "TYPE": "noise",
        "CATEGORY": "",
        "PARAMS": {
            "scale": {"UNIT": {"SUFFIX": "x", "SCALE": 1, "DECIMALS": 2}},
            "speed": {"DRIVE": "PHASE", "UNIT": {"SUFFIX": "/s", "SCALE": 1, "DECIMALS": 2}},
            "contrast": {"UNIT": {"SUFFIX": "x", "SCALE": 1, "DECIMALS": 2}},
            "red": {"UNIT": {"SUFFIX": "%", "SCALE": 100, "DECIMALS": 0}},
            "green": {"UNIT": {"SUFFIX": "%", "SCALE": 100, "DECIMALS": 0}},
            "blue": {"UNIT": {"SUFFIX": "%", "SCALE": 100, "DECIMALS": 0}}
        }
    },
    "INPUTS": [
        {"NAME": "scale", "TYPE": "float", "MIN": 0.5, "MAX": 32, "DEFAULT": 4},
        {"NAME": "speed", "TYPE": "float", "MIN": 0, "MAX": 4, "DEFAULT": 0.5},
        {"NAME": "contrast", "TYPE": "float", "MIN": 0, "MAX": 4, "DEFAULT": 1},
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
    vec3 tint = vec3(red, green, blue);
    if (contrast == 0.0) {
        gl_FragColor = vec4(tint * 0.5, 1.0);
        return;
    }
    vec2 xy = (px - RENDERSIZE * 0.5) * (scale / RENDERSIZE.y);
    vec3 q = vec3(0.8 * xy.x - 0.6 * xy.y, 0.6 * xy.x + 0.8 * xy.y, speed);
    vec3 i = floor(q), f = fract(q);
    f = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    uvec3 cell = uvec3(ivec3(i));
    float v[8];
    for (uint k = 0u; k < 8u; ++k) {
        uvec3 e = cell + uvec3(k & 1u, (k >> 1u) & 1u, k >> 2u);
        uint n = e.x * 0x8da6b343u + e.y * 0xd8163841u + e.z * 0xcb1ab31fu;
        n ^= n >> 15u;
        n *= 0x2c1b3c6du;
        n ^= n >> 12u;
        v[k] = float(n >> 8u) / 16777215.0;
    }
    vec4 z = mix(vec4(v[0], v[2], v[4], v[6]), vec4(v[1], v[3], v[5], v[7]), f.x);
    vec2 y = mix(z.xz, z.yw, f.y);
    float g = clamp((mix(y.x, y.y, f.z) - 0.5) * contrast + 0.5, 0.0, 1.0);
    gl_FragColor = vec4(tint * g, 1.0);
}
