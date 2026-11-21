/* gpuland.vert.hlsl -- the GPU ground (port/video/gpuland.c): a terrain point in the game's view
 * space projected as the game projects it (sx = half_w * (x / z + 1), sy = half_h * (y / z + 1),
 * in the sharp view's canvas), but with w = z, so the card interpolates the texture and the
 * light perspective-correct across every polygon (the game's linear bands swam).
 * SDL_GPU's layout for vertex shaders: uniform buffers in space 1; inputs named TEXCOORD<n>. */
#ifdef __spirv__
#define VK(x) [[vk::x]]
#else
#define VK(x)
#endif

VK(binding(0, 1))
cbuffer Camera : register(b0, space1) {
    float4 proj;    /* x_clip = proj.x * x + proj.y * z, y_clip = proj.z * y + proj.w * z */
    float4 depth;   /* z_clip = depth.x * z + depth.y */
};

struct VSInput {
    VK(location(0)) float4 a : TEXCOORD0;   /* x, y, z, light */
    VK(location(1)) float4 b : TEXCOORD1;   /* u, v (texels), texture layer (-1 - colour: flat), light table (-1: none) */
};

struct VSOutput {
    float4 pos : SV_Position;
    VK(location(0)) float2 uv : TEXCOORD0;
    VK(location(1)) float shade : TEXCOORD1;
    VK(location(2)) nointerpolation float2 sel : TEXCOORD2;
    VK(location(3)) float3 view : TEXCOORD3;     /* the point in view space (the fog's distance) */
};

VSOutput main(VSInput i)
{
    VSOutput o;
    float z = i.a.z;
    o.pos = float4(proj.x * i.a.x + proj.y * z, proj.z * i.a.y + proj.w * z, depth.x * z + depth.y, z);
    o.uv = i.b.xy;
    o.shade = i.a.w;
    o.sel = i.b.zw;
    o.view = i.a.xyz;
    return o;
}
