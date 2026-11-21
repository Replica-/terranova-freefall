/* gpuland_mix.frag.hlsl -- GPU rendering (port/video/gpuland.c) put into the view's picture: a
 * custom render state of SDL's GPU renderer for drawing the picture's texture (platform.c).
 * The display gives the 3D's pixels alpha 254, those that show the ground 253 and those of an object
 * the card drew 252, their colours kept.  On those the card's ground or objects are taken where the
 * card drew any (alpha 1), the picture's own colour where it drew none (a silhouette the two draw a
 * little differently).
 * 3 less (251, 250, 249) where an effects pack's picture shows over the pixel: the card's effects
 * layer (the pictures in their own colours and alpha, premultiplied) is laid over it.
 * Over every 3D pixel (not the HUD's) the card's weather is laid (premultiplied).  The 3D's
 * pixels then get the 3D's alpha (map.w: 254 for the anti-aliasing's mark, or 1); the others are
 * the picture as SDL would draw them.  The picture is drawn 1:1 or with nearest texels, so the
 * marks come through exactly.  Layout as common.hlsli: SDL's texture in space 2 slot 0, the extra
 * samplers (the ground, the weather, the objects, the effects) slots 1 to 4, the uniforms in
 * space 3. */
#ifdef __spirv__
#define VK(x) [[vk::x]]
#else
#define VK(x)
#endif

VK(combinedImageSampler) VK(binding(0, 2))
Texture2D u_texture : register(t0, space2);
VK(combinedImageSampler) VK(binding(0, 2))
SamplerState u_sampler : register(s0, space2);
VK(combinedImageSampler) VK(binding(1, 2))
Texture2D ground : register(t1, space2);
VK(combinedImageSampler) VK(binding(1, 2))
SamplerState ground_sampler : register(s1, space2);
VK(combinedImageSampler) VK(binding(2, 2))
Texture2D weather : register(t2, space2);
VK(combinedImageSampler) VK(binding(2, 2))
SamplerState weather_sampler : register(s2, space2);
VK(combinedImageSampler) VK(binding(3, 2))
Texture2D objects : register(t3, space2);
VK(combinedImageSampler) VK(binding(3, 2))
SamplerState objects_sampler : register(s3, space2);
VK(combinedImageSampler) VK(binding(4, 2))
Texture2D effects : register(t4, space2);
VK(combinedImageSampler) VK(binding(4, 2))
SamplerState effects_sampler : register(s4, space2);

VK(binding(0, 3))
cbuffer Map : register(b0, space3) {
    float4 map;     /* the card's texel: (u * map.x, v * map.y + map.z); map.w the 3D's alpha */
};

struct PSInput {
    VK(location(0)) float4 v_color : TEXCOORD0;
    VK(location(1)) float2 v_uv : TEXCOORD1;
};

float4 main(PSInput i) : SV_Target
{
    float4 c = u_texture.Sample(u_sampler, i.v_uv);
    float a = c.a * 255.0;
    if (a > 248.5 && a < 254.5) {
        float2 uv = float2(i.v_uv.x * map.x, i.v_uv.y * map.y + map.z);
        int d = 254 - (int)round(a);    /* 0 the 3D, 1 the ground, 2 an object; 3 more: an effect over it */
        int k = d >= 3 ? d - 3 : d;
        if (k == 2) {
            float4 g = objects.Sample(objects_sampler, uv);
            if (g.a > 0.5)
                c.rgb = g.rgb;
        } else if (k == 1) {
            float4 g = ground.Sample(ground_sampler, uv);
            if (g.a > 0.5)
                c.rgb = g.rgb;
        }
        if (d >= 3) {
            float4 e = effects.Sample(effects_sampler, uv);
            c.rgb = c.rgb * (1.0 - e.a) + e.rgb;
        }
        float4 w = weather.Sample(weather_sampler, uv);
        c.rgb = c.rgb * (1.0 - w.a) + w.rgb;
        c.a = map.w;
    }
    return c * i.v_color;
}
