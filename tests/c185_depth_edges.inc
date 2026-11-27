// Quarter-mask boundaries must not create a third, nonexistent surface.
void DepthCutouts(GLuint maskP, GLuint plain, GLuint guarded) {
    const char* const copyFs =
        "precision highp float;\nuniform sampler2D u_src;\nuniform vec4 u_offset;\nvarying vec2 v_uv;\n"
        "void main(){gl_FragColor=texture2D(u_src,v_uv+u_offset.xy);}\n";
    const auto copy = program(kVs, copyFs);
    Image background(W * H);
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x)
        background[y * W + x] = {static_cast<unsigned char>((x * 73 + x * x * 29 + 31) % 221 + 17),
                                static_cast<unsigned char>((x * 23 + y * 19 + 7) % 193 + 30),
                                static_cast<unsigned char>((x * 11 + y * 47 + 53) % 211 + 20), 255};
    const auto bg = texture(0, W, H, background.data(), true);
    auto overlay = [] {
        glEnable(GL_SCISSOR_TEST);
        glScissor(8, 8, 10, 14); glClearColor(.9f, .1f, .05f, 1); glClear(GL_COLOR_BUFFER_BIT);
        glScissor(98, 52, 26, 8); glClearColor(.1f, .85f, .2f, 1); glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);
    };
    int assets = 0;
    for (const auto fixture : {std::make_pair("C185_FLOWER_RGBA", 64), std::make_pair("C185_BUSH_RGBA", 128)}) {
        const char* path = std::getenv(fixture.first);
        if (!path) continue;
        FILE* f = std::fopen(path, "rb"); check(f, "open private cooked alpha fixture");
        Image rgba(fixture.second * fixture.second);
        check(std::fread(rgba.data(), sizeof(Pixel), rgba.size(), f) == rgba.size() && std::fgetc(f) == EOF,
              "private cooked alpha fixture size");
        check(std::fclose(f) == 0, "close private cooked alpha fixture");
        ++assets;
        for (int halfWidth : {24, 9}) {
            Image layer(W * H), source = background, rawDepth(W * H, Pixel{223, 223, 223, 255});
            for (int y = 8; y < 56; ++y) for (int x = 64 - halfWidth; x < 64 + halfWidth; ++x) {
                const int tx = (x - (64 - halfWidth)) * fixture.second / (2 * halfWidth);
                const int ty = (y - 8) * fixture.second / 48;
                auto p = rgba[(fixture.second - 1 - ty) * fixture.second + tx];
                check(p[3] == 0 || p[3] == 255, "fixture uses actual binary alpha cutouts");
                layer[y * W + x] = p;
                if (p[3]) { source[y * W + x] = p; rawDepth[y * W + x] = {128, 128, 128, 255}; }
            }
            const auto fg = texture(0, W, H, layer.data(), true), col = texture(0, W, H, source.data(), true);
            const auto mask = makeMask(maskP, rawDepth);
            double e0 = 0, e1 = 0;
            double t0 = 0, t1 = 0;
            int checked = 0, changed = 0;
            double worstRatio = 0;
            Image previousOff, previousOn, previousTruth;
            for (float pan : {-4.f, -2.f, -1.f, -.5f, -.25f, .25f, .5f, 1.f, 2.f, 4.f}) {
                const auto prefix = std::string(fixture.first) + "-" + std::to_string(2 * halfWidth) + "-" + std::to_string(pan);
                auto render = [&](GLuint p, const char* name) {
                    setup(p, col, mask); u(p, "u_z", pan / W); u(p, "u_p", 0, 64);
                    u(p, "u_m", 4.f / W, 4.f / H, W / 4, H / 4);
                    draw(); overlay(); return read((prefix + name).c_str());
                };
                const auto off = render(plain, "-linear"), on = render(guarded, "-guarded");
                setup(copy, bg, 0); u(copy, "u_offset", pan * (2.f - 223.f * (2.f / 255.f)) / W); draw();
                setup(copy, fg, 0); u(copy, "u_offset", pan * (2.f - 128.f * (2.f / 255.f)) / W);
                glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); draw(); glDisable(GL_BLEND);
                overlay(); const auto truth = read((prefix + "-truth").c_str());
                double p0 = 0, p1 = 0;
                for (int y = 4; y < H - 4; ++y) for (int x = 24; x < 100; ++x) {
                    p0 += err(off[y * W + x], truth[y * W + x]);
                    p1 += err(on[y * W + x], truth[y * W + x]);
                    changed += off[y * W + x] != on[y * W + x];
                    if (!previousOff.empty()) for (int channel = 0; channel < 3; ++channel) {
                        const int i = y * W + x;
                        const int dt = int(truth[i][channel]) - previousTruth[i][channel];
                        t0 += std::abs(int(off[i][channel]) - previousOff[i][channel] - dt);
                        t1 += std::abs(int(on[i][channel]) - previousOn[i][channel] - dt);
                    }
                    ++checked;
                }
                e0 += p0; e1 += p1;
                worstRatio = std::max(worstRatio, p1 / std::max(p0, 1.0));
                for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x)
                    if ((x >= 8 && x < 18 && y >= 8 && y < 22) || (x >= 98 && x < 124 && y >= 52 && y < 60))
                        check(off[y * W + x] == on[y * W + x] && on[y * W + x] == truth[y * W + x],
                              "fresh-player/HUD overlay pixels untouched");
                previousOff = off; previousOn = on; previousTruth = truth;
            }
            std::printf("%s width %d: %d pixels across 10 bidirectional slow pans, %d changed; independent layer redraw error %.3f -> %.3f levels/channel, worst pan ratio %.4f\n",
                        fixture.first, 2 * halfWidth, checked, changed, e0 / (3 * checked), e1 / (3 * checked), worstRatio);
            std::printf("  inter-pan residual change vs redraw: %.3f -> %.3f levels/channel; overlay differences zero\n",
                        t0 / (3 * checked * .9), t1 / (3 * checked * .9));
            check(e1 < e0 && worstRatio <= 1.0, "cooked cutout guard improves aggregate and every tested pan");
            check(t1 < t0, "cooked cutout temporal differential improves");
            glDeleteTextures(1, &fg); glDeleteTextures(1, &col); glDeleteTextures(1, &mask);
        }
    }
    if (std::getenv("C185_REQUIRE_SILHOUETTE")) check(assets == 2, "both cooked silhouette fixtures required");
    if (!assets) std::puts("cooked cutouts: skipped (private RGBA fixtures not supplied)");
    glDeleteTextures(1, &bg); glDeleteProgram(copy);
}
void DepthEdges() {
    // Frozen C184 output, commit cc43e986: no diagnostic must change existing shader/cache text.
    const uint64_t legacy[] = {0x28476979b89ab298ull, 0xf0e6252c4d2f9091ull, 0x15881b4b0229c5e7ull, 0xcba910205e28328aull,
                               0xf3b1f890d01bda69ull, 0x946f03cfebee4dfcull, 0x8639a1150599919aull, 0x73ff5128c6c6d3fbull};
    int variant = 0;
    for (depth41Mode = 1; depth41Mode <= 2; ++depth41Mode) for (int debug = 0; debug < 2; ++debug) for (int foreground = 0; foreground < 2; ++foreground) {
        depth41Debug = debug;
        depth41Edge = false;
        const auto source = D41Fragment(foreground);
        uint64_t hash = 14695981039346656037ull;
        for (unsigned char c : source) hash = (hash ^ c) * 1099511628211ull;
        check(hash == legacy[variant++], "disabled shader is byte-identical to frozen C184");
        depth41Edge = true;
        if (foreground) check(source == D41Fragment(true), "player occlusion shader unchanged with guard enabled");
        const auto enabled = D41Fragment(foreground);
        if (!foreground) {
            size_t samples = 0, calls = 0, at = 0;
            while ((at = enabled.find("texture2D(", at)) != std::string::npos) { ++samples; ++at; }
            at = 0;
            while ((at = enabled.find("depthCode(", at)) != std::string::npos) { ++calls; ++at; }
            check(samples == 3 && calls == static_cast<size_t>(depth41Mode + 1), "bounded optional mask-sample count");
        }
        const auto p = program(kVsWarp, enabled); glDeleteProgram(p);
    }
    depth41Mode = 1; depth41Debug = false; depth41Edge = false;
    std::puts("shader contract: all 8 disabled variants retain frozen C184 hashes; optional/occlusion variants compile");
    std::puts("sample contract: enabled scene mode 1 has 2 mask + 1 color reads; mode 2 has 4 mask + 1 color reads; disabled unchanged");
    const auto plainSource = D41Fragment();
    check(plainSource.find("u_m;") == std::string::npos && plainSource.find("depthCode(") == std::string::npos,
          "disabled depth guard adds no uniform or texture read");
    const auto maskP = program(kVs, MaskFragment()), d41 = program(kVsWarp, plainSource);
    depth41Edge = true;
    const auto edge = program(kVsWarp, D41Fragment());
    depth41Edge = false;
    Image source(W * H), depth(W * H);
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
        const auto c = static_cast<unsigned char>((x * 73 + x * x * 29 + 31) % 221 + 17);
        source[y * W + x] = {c, c, c, 255};
    }
    const auto col = texture(0, W, H, source.data(), true);
    double linearError = 0, nearestError = 0, edgeError = 0;
    int checks = 0;
    for (int boundary = 29; boundary <= 36; ++boundary) {
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
            const unsigned char z = y < boundary ? 128 : 223;
            depth[y * W + x] = {z, z, z, 255};
        }
        const auto mask = makeMask(maskP, depth);
        auto pan = [&](GLuint p, bool nearest) {
            setup(p, col, mask);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, nearest ? GL_NEAREST : GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, nearest ? GL_NEAREST : GL_LINEAR);
            u(p, "u_a", .5, 0, 0, .5f + 2.f / W);
            u(p, "u_z", 8.f / W);
            u(p, "u_p", 0, 64, 0, 0);
            u(p, "u_m", 4.f / W, 4.f / H, W / 4, H / 4);
            draw();
            return read(nearest ? "edge-nearest" : p == edge ? "edge-guarded" : "edge-linear");
        };
        const auto before = pan(d41, false), after = pan(d41, true), guarded = pan(edge, false);
        double e0 = 0, e1 = 0, e2 = 0;
        for (int y = boundary - 5; y < boundary + 5; ++y) for (int x = 8; x < W - 15; ++x) {
            const double raw = y < boundary ? 128.0 : 223.0;
            const double shift = 2.0 + 8.0 * (2.0 - raw * (2.0 / 255.0));
            const double s = x + shift;
            const int ix = static_cast<int>(std::floor(s));
            const double truth = source[y * W + ix][0] * (1.0 - (s - ix)) + source[y * W + ix + 1][0] * (s - ix);
            e0 += std::abs(before[y * W + x][0] - truth);
            e1 += std::abs(after[y * W + x][0] - truth);
            e2 += std::abs(guarded[y * W + x][0] - truth);
            ++checks;
        }
        std::printf("depth-edge row %d: linear %.3f, nearest %.3f, guarded %.3f absolute levels/pixel\n", boundary, e0 / (10 * (W - 23)), e1 / (10 * (W - 23)), e2 / (10 * (W - 23)));
        linearError += e0; nearestError += e1; edgeError += e2;
        glDeleteTextures(1, &mask);
    }
    std::printf("depth edges: %d checked pixels; mean absolute color error %.3f -> %.3f nearest, %.3f guarded levels/channel\n", checks, linearError / checks, nearestError / checks, edgeError / checks);
    check(edgeError < linearError * .8, "depth guard reduces phantom-depth band across all grid phases");
    for (int tilt = 0; tilt < 3; ++tilt) {
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
            const unsigned char z = tilt == 0 ? 191 : tilt == 1 ? 128 + x / 2 : 150 + y;
            depth[y * W + x] = {z, z, z, 255};
        }
        const auto mask = makeMask(maskP, depth);
        for (float pan : {-8.f, -1.f, -.25f, .25f, 1.f, 8.f}) {
            auto render = [&](GLuint p) {
                setup(p, col, mask);
                u(p, "u_a", .5, 0, 0, .5f + .375f / W);
                u(p, "u_b", 0, .5, 0, .5f + .125f / H);
                u(p, "u_z", pan / W, pan / (3 * H));
                u(p, "u_p", 0, 64);
                u(p, "u_m", 4.f / W, 4.f / H, W / 4, H / 4);
                draw(); return read("smooth-depth");
            };
            const auto off = render(d41), on = render(edge);
            check(off == on, "flat/tilted surfaces unchanged during slow/fast pans in both directions");
        }
        glDeleteTextures(1, &mask);
    }
    std::puts("flat/tilted surfaces: all framebuffer bytes identical across 18 diagonal/bidirectional pan cases");
    DepthCutouts(maskP, d41, edge);
    glDeleteTextures(1, &col);
    glDeleteProgram(maskP); glDeleteProgram(d41);
    glDeleteProgram(edge);
}
