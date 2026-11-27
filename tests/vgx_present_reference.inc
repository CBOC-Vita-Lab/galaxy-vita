    static const char* vs =
        "attribute vec4 a_pos;\nuniform vec4 u_rect;\nvarying vec2 v_uv;\n"
        "void main() { v_uv = vec2(u_rect.x + (a_pos.x * 0.5 + 0.5) * u_rect.z, u_rect.y + (a_pos.y * 0.5 + 0.5) * u_rect.w); gl_Position = vec4(a_pos.xy, 0.0, 1.0); }\n";
    static const char* fs =
        "precision highp float;\nuniform sampler2D u_src;\nuniform vec4 u_size;\nuniform vec4 u_lim;\nuniform vec4 u_p;\nvarying vec2 v_uv;\nvoid main() {\n"
        "  vec2 uv = clamp(v_uv, u_lim.xy, u_lim.zw);\n"
        "  vec4 c = texture2D(u_src, uv);\n"
        "  vec4 b = texture2D(u_src, clamp(uv + vec2(u_size.z, 0.0), u_lim.xy, u_lim.zw)) + texture2D(u_src, clamp(uv - vec2(u_size.z, 0.0), u_lim.xy, u_lim.zw))\n"
        "         + texture2D(u_src, clamp(uv + vec2(0.0, u_size.w), u_lim.xy, u_lim.zw)) + texture2D(u_src, clamp(uv - vec2(0.0, u_size.w), u_lim.xy, u_lim.zw));\n"
        "  c = c + (c - b * 0.25) * u_p.z;\n"
        "  gl_FragColor = vec4(c.rgb, 1.0);\n}\n";
