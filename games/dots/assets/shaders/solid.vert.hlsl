cbuffer ViewUniforms : register(b0, space1) {
    float4 output_size;
};

struct VertexInput {
    float2 corner : TEXCOORD0;
    float2 center : TEXCOORD1;
    float2 half_size : TEXCOORD2;
    float4 color : TEXCOORD3;
};

struct VertexOutput {
    float4 position : SV_Position;
    float4 color : TEXCOORD0;
};

VertexOutput main(VertexInput input) {
    const float2 screen = input.center + (input.corner * input.half_size);

    VertexOutput output;
    output.position = float4(
        ((screen.x / output_size.x) * 2.0) - 1.0,
        1.0 - ((screen.y / output_size.y) * 2.0),
        0.0,
        1.0);
    output.color = input.color;
    return output;
}
