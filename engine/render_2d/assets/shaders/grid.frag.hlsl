cbuffer GridUniforms : register(b0, space3) {
    float4 camera_and_output_width;
    float4 output_height_scale_spacing;
    float4 background_color;
    float4 grid_color;
};

float4 main(float4 position : SV_Position) : SV_Target0 {
    const float2 camera = camera_and_output_width.xy;
    const float2 output_size =
        float2(camera_and_output_width.z, output_height_scale_spacing.x);
    const float pixels_per_world_unit = output_height_scale_spacing.y;
    const float grid_spacing = output_height_scale_spacing.z;
    const float2 world = ((position.xy - (output_size * 0.5)) / pixels_per_world_unit) + camera;
    const float2 cell = abs(frac((world / grid_spacing) + 0.5) - 0.5);
    const float distance_in_pixels = min(cell.x, cell.y) * grid_spacing * pixels_per_world_unit;
    const float grid_amount = 1.0 - smoothstep(0.5, 1.5, distance_in_pixels);
    return lerp(background_color, grid_color, grid_amount);
}
