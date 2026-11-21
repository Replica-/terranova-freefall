/* gpufx.frag.hlsl -- GPU rendering's effects (port/video/gpuland.c, gpufx.vert.hlsl): the
 * picture's colour and alpha filtered (smooth when scaled), lit as the game lights the sprite
 * (its light row as colour * a + b), its alpha times the opacity, premultiplied for the layer
 * (one, one - alpha: the effects over each other in the game's order).
 * SDL_GPU's layout for fragment shaders: textures and samplers in space 2. */
#ifdef __spirv__
#define VK(x) [[vk::x]]
#else
#define VK(x)
#endif

VK(binding(0, 2)) Texture2DArray pics : register(t0, space2);
VK(binding(1, 2)) SamplerState lin : register(s0, space2);

struct PSInput {
    float4 pos : SV_Position;
    VK(location(0)) float3 uvl : TEXCOORD0;
    VK(location(1)) nointerpolation float4 fa : TEXCOORD1;
    VK(location(2)) nointerpolation float4 fb : TEXCOORD2;
};

float4 main(PSInput i) : SV_Target0
{
    float4 p = pics.Sample(lin, i.uvl);
    float3 c = i.fb.w > 0.5 ? saturate(p.rgb * i.fa.rgb + i.fb.rgb) : p.rgb;
    float a = p.a * i.fa.w;
    if (a < 1.0 / 255.0)
        discard;
    return float4(c * a, a);
}
