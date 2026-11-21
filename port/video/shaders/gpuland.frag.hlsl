/* gpuland.frag.hlsl -- the GPU ground (port/video/gpuland.c): a texel's colour number through
 * the game's light table and palette, as the game's lit mappers do (pixel = ltab[row << 8 |
 * texel]), in full colour:
 *   - the light runs smooth between the table's rows (the game steps from row to row);
 *   - params.x = 1: the texture filtered (the four texels around the point, each lit and
 *     coloured, mixed), 0: nearest, the game's square texels.
 * SDL_GPU's layout for fragment shaders: textures and samplers in space 2, uniforms in space 3. */
#ifdef __spirv__
#define VK(x) [[vk::x]]
#else
#define VK(x)
#endif

VK(binding(0, 2)) Texture2DArray<float> texels : register(t0, space2);    /* 64 x 64 colour numbers / 255 */
VK(binding(1, 2)) Texture2DArray<float> ltabs : register(t1, space2);     /* 256 x 24: colour number by light row */
VK(binding(2, 2)) Texture2D<float4> pal : register(t2, space2);           /* 256 x 1 */
VK(binding(3, 2)) SamplerState s0 : register(s0, space2);
VK(binding(4, 2)) SamplerState s1 : register(s1, space2);
VK(binding(5, 2)) SamplerState s2 : register(s2, space2);

VK(binding(0, 3))
cbuffer Look : register(b0, space3) {
    float4 params;
};

struct PSInput {
    float4 pos : SV_Position;
    VK(location(0)) float2 uv : TEXCOORD0;
    VK(location(1)) float shade : TEXCOORD1;
    VK(location(2)) nointerpolation float2 sel : TEXCOORD2;
};

int colour_at(int2 t, int layer)
{
    return (int)round(texels.Load(int4(t & 63, layer, 0)) * 255.0);
}

/* colour number c at light L (rows, fractional) through table lt */
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
    int layer = (int)round(i.sel.x);
    int lt = (int)round(i.sel.y);
    /* the game's row is the light's whole part: half a row down makes the mix centred on it */
    float L = i.shade - 0.5;
    if (layer < 0)
        return float4(lit_colour(-1 - layer, L, lt), 1.0);
    if (params.x < 0.5)
        return float4(lit_colour(colour_at(int2(floor(i.uv)), layer), L, lt), 1.0);
    float2 p = i.uv - 0.5;
    int2 t = int2(floor(p));
    float2 f = p - floor(p);
    float3 c00 = lit_colour(colour_at(t, layer), L, lt);
    float3 c10 = lit_colour(colour_at(t + int2(1, 0), layer), L, lt);
    float3 c01 = lit_colour(colour_at(t + int2(0, 1), layer), L, lt);
    float3 c11 = lit_colour(colour_at(t + int2(1, 1), layer), L, lt);
    return float4(lerp(lerp(c00, c10, f.x), lerp(c01, c11, f.x), f.y), 1.0);
}
