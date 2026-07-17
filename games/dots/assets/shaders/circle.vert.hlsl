cbuffer ViewUniforms : register(b0, space1) {
    float4 camera_and_output_width;
    float4 output_height_and_scale;
};

struct VertexInput {
    float2 corner : TEXCOORD0;
    float2 center : TEXCOORD1;
    float radius : TEXCOORD2;
    float4 color : TEXCOORD3;
};

struct VertexOutput {
    float4 position : SV_Position;
    float2 circle_coordinate : TEXCOORD0;
    float4 color : TEXCOORD1;
};

VertexOutput main(VertexInput input) {
    const float2 camera = camera_and_output_width.xy;
    const float2 output_size = float2(camera_and_output_width.z, output_height_and_scale.x);
    const float pixels_per_world_unit = output_height_and_scale.y;
    const float2 screen =
        ((input.center + (input.corner * input.radius) - camera) * pixels_per_world_unit) +
        (output_size * 0.5);

    VertexOutput output;
    output.position = float4(
        ((screen.x / output_size.x) * 2.0) - 1.0,
        1.0 - ((screen.y / output_size.y) * 2.0),
        0.0,
        1.0);
    output.circle_coordinate = input.corner;
    output.color = input.color;
    return output;
}
