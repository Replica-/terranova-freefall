/* gpuobj.frag.hlsl -- GPU rendering's objects (port/video/gpuland.c, gpuobj.vert.hlsl): the
 * texel's colour number (the texture's own size, wrapping as the game's mappers do), see-through
 * where 0 when the texture is, through the game's light table and palette in full colour, the
 * light smooth between the table's rows (gpuland.frag.hlsl's lit_colour).
 * SDL_GPU's layout for fragment shaders: textures and samplers in space 2. */
#ifdef __spirv__
#define VK(x) [[vk::x]]
#else
#define VK(x)
#endif

VK(binding(0, 2)) Texture2DArray<float> otex : register(t0, space2);     /* 256 x 256 slots, colour numbers / 255 */
VK(binding(1, 2)) Texture2DArray<float> ltabs : register(t1, space2);    /* 256 x 24 */
VK(binding(2, 2)) Texture2D<float4> pal : register(t2, space2);          /* 256 x 1 */
VK(binding(3, 2)) SamplerState s0 : register(s0, space2);
VK(binding(4, 2)) SamplerState s1 : register(s1, space2);
VK(binding(5, 2)) SamplerState s2 : register(s2, space2);

struct PSInput {
    float4 pos : SV_Position;
    VK(location(0)) float2 uv : TEXCOORD0;
    VK(location(1)) float shade : TEXCOORD1;
    VK(location(2)) nointerpolation float2 sel : TEXCOORD2;
    VK(location(3)) nointerpolation float3 tex : TEXCOORD3;
};

float3 lit_colour(int c, float L, int lt)
{
    if (lt < 0)
        return pal.Load(int3(c, 0, 0)).rgb;
    L = clamp(L, 0.0, 23.0);
    int r0 = (int)floor(L);
    int r1 = min(r0 + 1, 23);
    int p0 = (int)round(ltabs.Load(int4(c, r0, lt, 0)) * 255.0);
    int p1 = (int)round(ltabs.Load(int4(c, r1, lt, 0)) * 255.0);
    return lerp(pal.Load(int3(p0, 0, 0)).rgb, pal.Load(int3(p1, 0, 0)).rgb, L - r0);
}

float4 main(PSInput i) : SV_Target0
{
    int2 size = int2(round(i.tex.xy));
    int2 t = int2(floor(i.uv));
    t = ((t % size) + size) % size;
    int c = (int)round(otex.Load(int4(t, (int)round(i.sel.x), 0)) * 255.0);
    if (i.tex.z > 0.5 && c == 0)
        discard;
    return float4(lit_colour(c, i.shade - 0.5, (int)round(i.sel.y)), 1.0);
}
