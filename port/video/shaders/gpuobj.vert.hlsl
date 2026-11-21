/* gpuobj.vert.hlsl -- GPU rendering's objects (port/video/gpuland.c): an object's textured polygon
 * as the 3D library drew it into the sharp canvas, its point turned back into the view space the
 * ground's projection takes (gpuland.vert.hlsl: w = z, the texture perspective-correct).  The
 * objects are drawn in the order the game drew them (far to near), so no depth.
 * SDL_GPU's layout for vertex shaders: uniform buffers in space 1; inputs named TEXCOORD<n>. */
#ifdef __spirv__
#define VK(x) [[vk::x]]
#else
#define VK(x)
#endif

VK(binding(0, 1))
cbuffer Camera : register(b0, space1) {
    float4 proj;
    float4 depth;
};

struct VSInput {
    VK(location(0)) float4 a : TEXCOORD0;   /* x, y, z, light */
    VK(location(1)) float4 b : TEXCOORD1;   /* u, v (texels), texture layer, light table (-1: none) */
    VK(location(2)) float4 c : TEXCOORD2;   /* the texture's width, height, see-through */
};

struct VSOutput {
    float4 pos : SV_Position;
    VK(location(0)) float2 uv : TEXCOORD0;
    VK(location(1)) float shade : TEXCOORD1;
    VK(location(2)) nointerpolation float2 sel : TEXCOORD2;
    VK(location(3)) nointerpolation float3 tex : TEXCOORD3;
};

VSOutput main(VSInput i)
{
    VSOutput o;
    float z = i.a.z;
    o.pos = float4(proj.x * i.a.x + proj.y * z, proj.z * i.a.y + proj.w * z, 0.5 * z, z);
    o.uv = i.b.xy;
    o.shade = i.a.w;
    o.sel = i.b.zw;
    o.tex = i.c.xyz;
    return o;
}
