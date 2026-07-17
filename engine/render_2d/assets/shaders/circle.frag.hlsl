struct FragmentInput {
    float4 position : SV_Position;
    float2 circle_coordinate : TEXCOORD0;
    float4 color : TEXCOORD1;
};

float4 main(FragmentInput input) : SV_Target0 {
    const float distance_squared = dot(input.circle_coordinate, input.circle_coordinate);
    const float edge_width = max(fwidth(distance_squared), 0.0001);
    const float coverage = 1.0 - smoothstep(1.0 - edge_width, 1.0, distance_squared);
    return float4(input.color.rgb, input.color.a * coverage);
}
