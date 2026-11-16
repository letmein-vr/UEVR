// Borderlands 4 reticle colour key (Borderlands4ReticleLayer.hpp).
//
// The profile Lua sets the game's three crosshair colours to keys no other HUD element uses: pure red/blue mixes with
// zero green (neutral R = B, enemy B = 0.12 R, friendly R = 0.12 B). These SpriteBatch pixel shaders read the centre of the
// game UI texture (premultiplied alpha):
//   SpritePixelShaderExtract: keeps only the pixels of one key category (color.a * 4 = 1 neutral, 2 enemy, 3 friendly)
//                             and recolours them to color.rgb (the player's real crosshair colour) - drawn into the
//                             reticle layer with premultiplied-alpha blending, once per category.
//   SpritePixelShaderRemove:  writes every keyed pixel as transparent (alpha = color.a, the UI's transparent value
//                             after UEVR's alpha inversion) and passes everything else through unchanged - drawn back
//                             over the UI with blending off. Item cards, prompts and damage numbers are never touched.
// Build: compile_shaders.bat (writes Compiled\bl4_reticle_key_*.inc).

#define SpriteStaticRS \
"RootFlags ( ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |" \
"            DENY_DOMAIN_SHADER_ROOT_ACCESS |" \
"            DENY_GEOMETRY_SHADER_ROOT_ACCESS |" \
"            DENY_HULL_SHADER_ROOT_ACCESS )," \
"DescriptorTable ( SRV(t0), visibility = SHADER_VISIBILITY_PIXEL ),"\
"CBV(b0), "\
"StaticSampler(s0,"\
"           filter = FILTER_MIN_MAG_MIP_POINT,"\
"           addressU = TEXTURE_ADDRESS_CLAMP,"\
"           addressV = TEXTURE_ADDRESS_CLAMP,"\
"           addressW = TEXTURE_ADDRESS_CLAMP,"\
"           visibility = SHADER_VISIBILITY_PIXEL )"

Texture2D<float4> Texture : register(t0);
sampler TextureSampler : register(s0);

cbuffer Parameters : register(b0)
{
    row_major float4x4 MatrixTransform;
};

[RootSignature(SpriteStaticRS)]
void SpriteVertexShader(inout float4 color    : COLOR0,
    inout float2 texCoord : TEXCOORD0,
    inout float4 position : SV_Position)
{
    position = mul(position, MatrixTransform);
}

// 0 = not a key colour, 1 = neutral, 2 = enemy, 3 = friendly. Works on premultiplied values: the red/blue ratio and the
// missing green survive antialiasing and glow alpha. The game does not draw the keys exactly: neutral (1,0,1) arrives
// as about (0.71, 0.05, 0.88) - ratio 0.81, a little green (measured 2026-11-09) - so the ranges are wide. Enemy /
// friendly keys are 0.12 on the weak channel: 0.12 linear = 0.38 sRGB, times that ~0.8 skew = 0.1..0.47.
float KeyCategory(float3 c)
{
    const float m = max(c.r, c.b);

    if (m < 0.012) {
        return 0.0;
    }

    if (c.g > 0.12 * m + 0.01) {
        return 0.0;
    }

    const float q = min(c.r, c.b) / m;

    if (q > 0.68) {
        return 1.0;
    }

    if (q > 0.05 && q < 0.6) {
        return c.r > c.b ? 2.0 : 3.0;
    }

    return 0.0;
}

[RootSignature(SpriteStaticRS)]
float4 SpritePixelShaderExtract(float4 color    : COLOR0,
    float2 texCoord : TEXCOORD0) : SV_Target0
{
    const float4 tex = Texture.Sample(TextureSampler, texCoord);

    if (tex.a < 0.003 || KeyCategory(tex.rgb) != round(color.a * 4.0)) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    // brightness of the key relative to full coverage (1 for a pure key pixel)
    const float intensity = saturate(max(tex.r, tex.b) / max(tex.a, 0.0001));
    return float4(color.rgb * intensity * tex.a, tex.a);
}

[RootSignature(SpriteStaticRS)]
float4 SpritePixelShaderRemove(float4 color    : COLOR0,
    float2 texCoord : TEXCOORD0) : SV_Target0
{
    const float4 tex = Texture.Sample(TextureSampler, texCoord);

    if (KeyCategory(tex.rgb) > 0.0) {
        return float4(0.0, 0.0, 0.0, color.a);
    }

    return tex;
}
