/*{
    "CREDIT": "Nocturne, AGPL-3.0-or-later",
    "DESCRIPTION": "Sobel gradient magnitude per colour channel, scaled by the multiplier",
    "NOCTURNE": {
        "TYPE": "edgeDetection",
        "CATEGORY": "",
        "PARAMS": {"multiplier": {"UNIT": {"SUFFIX": "x", "SCALE": 1, "DECIMALS": 2}}}
    },
    "INPUTS": [
        {"NAME": "inputImage", "TYPE": "image"},
        {"NAME": "multiplier", "TYPE": "float", "MIN": 0, "MAX": 2, "DEFAULT": 0.25}
    ],
    "ISFVSN": "2"
}
*/

void main()
{
    vec2 px = isf_FragNormCoord * RENDERSIZE;
    float a = IMG_PIXEL(inputImage, px).a;
    if (multiplier == 0.0) {
        gl_FragColor = vec4(0.0, 0.0, 0.0, a);
        return;
    }
    vec3 k[9];
    for (int j = 0; j < 3; ++j)
        for (int i = 0; i < 3; ++i)
            k[j * 3 + i] = IMG_PIXEL(inputImage, px + vec2(i - 1, 1 - j)).rgb;
    vec3 gx = k[2] + 2.0 * k[5] + k[8] - k[0] - 2.0 * k[3] - k[6];
    vec3 gy = k[6] + 2.0 * k[7] + k[8] - k[0] - 2.0 * k[1] - k[2];
    gl_FragColor = vec4(clamp(sqrt(gx * gx + gy * gy) * multiplier, 0.0, 1.0), a);
}
