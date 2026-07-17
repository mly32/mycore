cbuffer GridUniforms : register(b0, space3) {
    float4 camera_and_output_width;
    float4 output_height_scale_spacing_enabled;
    float4 background_color;
    float4 grid_color;
};

float4 main(float4 position : SV_Position) : SV_Target0 {
    const float2 camera = camera_and_output_width.xy;
    const float2 output_size =
        float2(camera_and_output_width.z, output_height_scale_spacing_enabled.x);
    const float pixels_per_world_unit = output_height_scale_spacing_enabled.y;
    const float grid_spacing = output_height_scale_spacing_enabled.z;
    const bool grid_enabled = output_height_scale_spacing_enabled.w > 0.5;

    if (!grid_enabled || grid_spacing <= 0.0 || pixels_per_world_unit <= 0.0) {
        return background_color;
    }

    const float2 world = ((position.xy - (output_size * 0.5)) / pixels_per_world_unit) + camera;
    const float2 cell = abs(frac((world / grid_spacing) + 0.5) - 0.5);
    const float distance_in_pixels = min(cell.x, cell.y) * grid_spacing * pixels_per_world_unit;
    const float grid_amount = 1.0 - smoothstep(0.5, 1.5, distance_in_pixels);
    return lerp(background_color, grid_color, grid_amount);
}
