/* gpufx.vert.hlsl -- GPU rendering's effects (port/video/gpuland.c): an effects pack's picture
 * (explosions, smoke) as the game placed it on the sharp canvas, a rectangle, drawn from the
 * picture's own colours and alpha (the game's drawing maps them to its 256 colours).
 * SDL_GPU's layout for vertex shaders: uniform buffers in space 1; inputs named TEXCOORD<n>. */
#ifdef __spirv__
#define VK(x) [[vk::x]]
#else
#define VK(x)
#endif

VK(binding(0, 1))
cbuffer Canvas : register(b0, space1) {
    float4 size;        /* the canvas's width, height */
};

struct VSInput {
    VK(location(0)) float4 a : TEXCOORD0;   /* x, y (canvas pixels), its picture's slot, 0 */
    VK(location(1)) float4 b : TEXCOORD1;   /* u, v (in the slot, 0 .. 1), 0, 0 */
    VK(location(2)) float4 c : TEXCOORD2;   /* the light's fit a (rgb), the opacity */
    VK(location(3)) float4 d : TEXCOORD3;   /* the light's fit b (rgb), lit */
};

struct VSOutput {
    float4 pos : SV_Position;
    VK(location(0)) float3 uvl : TEXCOORD0;
    VK(location(1)) nointerpolation float4 fa : TEXCOORD1;
    VK(location(2)) nointerpolation float4 fb : TEXCOORD2;
};

VSOutput main(VSInput i)
{
    VSOutput o;
    o.pos = float4(i.a.x / size.x * 2.0 - 1.0, 1.0 - i.a.y / size.y * 2.0, 0.5, 1.0);
    o.uvl = float3(i.b.xy, i.a.z);
    o.fa = i.c;
    o.fb = i.d;
    return o;
}
