/*{
    "CREDIT": "Nocturne, AGPL-3.0-or-later",
    "DESCRIPTION": "White specks on black, denser where the picture is brighter, drawn anew 24 times a second",
    "NOCTURNE": {"TYPE": "grain", "CATEGORY": ""},
    "INPUTS": [
        {
            "NAME": "inputImage",
            "TYPE": "image"
        }
    ],
    "ISFVSN": "2"
}
*/

uint pcg(uint n)
{
    n = n * 747796405u + 2891336453u;
    n = ((n >> ((n >> 28u) + 4u)) ^ n) * 277803737u;
    return (n >> 22u) ^ n;
}

void main()
{
    vec4 c = IMG_THIS_PIXEL(inputImage);
    vec2 px = vec2(gl_FragCoord.x, RENDERSIZE.y - gl_FragCoord.y);
    float y = dot(c.rgb, vec3(0.2126, 0.7152, 0.0722));
    uint n = pcg(uint(px.x) * 1973u + uint(px.y) * 9277u + uint(TIME * 24.0) * 26699u);
    float speck = float(n >> 8u) / 16777215.0 < smoothstep(0.05, 0.6, y) ? 1.0 : 0.0;
    gl_FragColor = vec4(vec3(speck), c.a);
}
