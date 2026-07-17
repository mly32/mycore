struct FragmentInput {
    float4 position : SV_Position;
    float2 circle_coordinate : TEXCOORD0;
    float4 color : TEXCOORD1;
    float4 outline_color : TEXCOORD2;
    float inner_radius_squared : TEXCOORD3;
};

float4 main(FragmentInput input) : SV_Target0 {
    const float distance_squared = dot(input.circle_coordinate, input.circle_coordinate);
    const float edge_width = max(fwidth(distance_squared), 0.0001);
    const float outer_coverage = 1.0 - smoothstep(1.0 - edge_width, 1.0, distance_squared);
    const float inner_coverage =
        1.0 - smoothstep(input.inner_radius_squared - edge_width,
                         input.inner_radius_squared + edge_width,
                         distance_squared);
    const float4 color = lerp(input.outline_color, input.color, inner_coverage);
    return float4(color.rgb, color.a * outer_coverage);
}
