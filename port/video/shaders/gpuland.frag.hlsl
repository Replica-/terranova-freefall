/* gpuland.frag.hlsl -- the GPU ground (port/video/gpuland.c): a texel's colour number through
 * the game's light table and palette, as the game's lit mappers do (pixel = ltab[row << 8 |
 * texel]), in full colour, the light smooth between the table's rows (the game steps from row to
 * row).  The texture, by how many of its texels fall on a pixel:
 *   - near (a texel over a pixel or more): the game's square texels with their edges smoothed
 *     over one pixel ("crisp": sharp as the game's, but no stair steps or crawling edges), each
 *     of the four texels lit exactly through the table;
 *   - far (more texels than pixels): the texture's full-colour mipmaps (gpuland.c makes them from
 *     the colour numbers), lit by the table row's fit (colour * a + b, per channel: the game's
 *     light and fog rows are close to that), so the far ground doesn't shimmer;
 *   - between: the two mixed.
 * params.x = 0: the game's texels as they are (nearest, no mipmaps).
 * SDL_GPU's layout for fragment shaders: textures and samplers in space 2, uniforms in space 3. */
#ifdef __spirv__
#define VK(x) [[vk::x]]
#else
#define VK(x)
#endif

VK(binding(0, 2)) Texture2DArray<float> texels : register(t0, space2);    /* 64 x 64 colour numbers / 255 */
VK(binding(1, 2)) Texture2DArray<float> ltabs : register(t1, space2);     /* 256 x 24: colour number by light row */
VK(binding(2, 2)) Texture2D<float4> pal : register(t2, space2);           /* 256 x 1 */
VK(binding(3, 2)) Texture2DArray<float4> colours : register(t3, space2);  /* the textures in colour, mipmapped */
VK(binding(4, 2)) Texture2DArray<float4> fits : register(t4, space2);     /* 2 x 24: each row's a, b */
VK(binding(5, 2)) SamplerState s0 : register(s0, space2);
VK(binding(6, 2)) SamplerState s1 : register(s1, space2);
VK(binding(7, 2)) SamplerState s2 : register(s2, space2);
VK(binding(8, 2)) SamplerState trilinear : register(s3, space2);
VK(binding(9, 2)) SamplerState s4 : register(s4, space2);

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

/* a full colour at light L through table lt's fits */
float3 lit_fit(float3 c, float L, int lt)
{
    if (lt < 0)
        return c;
    L = clamp(L, 0.0, 23.0);
    int r0 = (int)floor(L);
    int r1 = min(r0 + 1, 23);
    float3 c0 = c * fits.Load(int4(0, r0, lt, 0)).rgb + fits.Load(int4(1, r0, lt, 0)).rgb;
    float3 c1 = c * fits.Load(int4(0, r1, lt, 0)).rgb + fits.Load(int4(1, r1, lt, 0)).rgb;
    return saturate(lerp(c0, c1, L - r0));
}

float4 main(PSInput i) : SV_Target0
{
    int layer = (int)round(i.sel.x);
    int lt = (int)round(i.sel.y);
    /* the game's row is the light's whole part: half a row down makes the mix centred on it */
    float L = i.shade - 0.5;
    /* texels per pixel (before any branch: the derivatives need every pixel of the quad) */
    float2 dx = ddx(i.uv), dy = ddy(i.uv);
    float tpp = sqrt(length(dx) * length(dy));      /* (a low angle stretches one way only) */
    float2 fw = max(abs(dx) + abs(dy), 1e-4);
    if (layer < 0)
        return float4(lit_colour(-1 - layer, L, lt), 1.0);
    if (params.x < 0.5)
        return float4(lit_colour(colour_at(int2(floor(i.uv)), layer), L, lt), 1.0);

    float3 nearc = 0, farc = 0;
    float far_k = saturate(log2(max(tpp, 1e-4)) + 0.5);     /* 0 up to 0.7 texels a pixel, 1 from 1.4 */
    if (far_k < 1.0) {
        /* crisp: the four texels around the point, the mix's step squeezed into one pixel */
        float2 p = i.uv - 0.5;
        int2 t = int2(floor(p));
        float2 f = saturate((p - floor(p) - 0.5) / fw + 0.5);
        float3 c00 = lit_colour(colour_at(t, layer), L, lt);
        float3 c10 = lit_colour(colour_at(t + int2(1, 0), layer), L, lt);
        float3 c01 = lit_colour(colour_at(t + int2(0, 1), layer), L, lt);
        float3 c11 = lit_colour(colour_at(t + int2(1, 1), layer), L, lt);
        nearc = lerp(lerp(c00, c10, f.x), lerp(c01, c11, f.x), f.y);
    }
    if (far_k > 0.0)
        farc = lit_fit(colours.SampleGrad(trilinear, float3(i.uv / 64.0, layer), dx / 64.0, dy / 64.0).rgb, L, lt);
    return float4(lerp(nearc, farc, far_k), 1.0);
}
