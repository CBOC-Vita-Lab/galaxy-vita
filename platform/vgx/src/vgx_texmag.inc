// GXV_TEXMAG_CENSUS=N (hdcensus): a measurement mode. For one picture in N, every textured draw of the finished command buffer
// (g_buf, in FrontPresent, after the finishing thread's work has joined) gets up to GXV_TEXMAG_TRIS (16) of its triangles
// transformed on the CPU with the draw's own uniform blocks (the vertex shader's arithmetic: u_mv / u_pal, u_proj, texgen
// matrices), clipped (near / far in clip space, then the scissor rectangle on the screen) and measured: the clipped triangle's area
// in display pixels (default 960x544) against its area in texels of the texture. The ratio gives the magnification m (screen
// pixels per texel, linear) and the mip level the GPU would pick, lod = log2(texels per pixel) = -log2(m), per texture.
// Nothing here writes to g_buf or to any state the front end reads; with N = 0 the only cost is one predictable branch per picture.
// Limits (also in the report): no depth test (occluded surfaces are counted: the per-texture share of the screen is logged);
// environment / projective / indirect / colour-generated texture coordinates are skipped (counted); the ratio is the clipped
// piece's mean (area ratio) - a sampled triangle larger than GXV_TEXMAG_PIECE_PX (6000) display pixels is split in four, up to
// GXV_TEXMAG_SUBDIV (3) times and GXV_TEXMAG_PIECES (4000) a picture, so that a tilted floor's near and far parts land in their own bins; draws of isolated passes and of small viewports (not the picture) are skipped.
// Thread: everything runs on the front-end (player) thread. GXV_FE_TWOCORE declines. The file is written by a writer thread.
#if defined(VGX_CLONE_B)
inline void TexMagFrame() {}
#else
namespace texmag {

constexpr uint32_t kMaxTex = 4096, kFileLines = 640, kLogLines = 64, kMaxCoord = 4;

int g_armed = -1; // -1 not read, 0 off, 1 on (front-end thread)
bool g_anyWrite = false; // GXV_TEXMAG_ANYWRITE=1 (a test knob: the host fixtures' draws never set the colour-write bits)
long g_every = 0, g_triPer = 16, g_triBudget = 8192, g_minSide = 16, g_writeSec = 60;
size_t g_shadowCap = 12u << 20;
int g_subdiv = 3;            // a sampled triangle larger than g_pieceArea display pixels is split in four, up to this many times
double g_pieceArea = 6000;
uint32_t g_pieces = 0, g_pieceCap = 4000; // (this picture's splits so far, and the cap)
int g_scrW = 960, g_scrH = 544;
std::string g_dir;

struct TexInfo { uint64_t key = 0; uint16_t w = 0, h = 0; uint8_t fmt = 0, player = 0; };
std::vector<TexInfo> g_texInfo; // by back-end texture id; key 0 = not a guest texture of the census's

struct Acc {
    uint64_t key = 0;
    uint16_t w = 0, h = 0;
    uint8_t fmt = 0, player = 0;
    uint32_t pics = 0, curPic = 0;
    double px[5] = {};       // display pixels over all sampled pictures: all, m >= 1, 1.5, 2, 3
    double mini[3] = {};     // minified: lod >= 0.5, 1, 2 (m <= 0.707, 0.5, 0.25)
    double sumMag = 0, sumLod = 0, sumLodMin = 0, maxMag = 0, minMag = 1e30;
    double curAll = 0, curPx15 = 0, peakPx15 = 0, peakShare = 0;
};
std::unordered_map<uint64_t, Acc> g_acc;
std::vector<Acc*> g_touched;

struct Ctr {
    uint64_t seen = 0, sampled = 0, draws = 0, texDraws = 0, measured = 0, tris = 0, trisCulled = 0, trisOff = 0, trisBad = 0, ns = 0, nsMax = 0;
    uint64_t sNoTex = 0, sTiny = 0, sEnv = 0, sProj = 0, sInd = 0, sNoCoord = 0, sNoStatic = 0, sBlock = 0, sPass = 0, sVp = 0, sDepth = 0, sDegen = 0, sCapped = 0;
    double totalPx = 0;
} g_c;

// shadow copies of the static buffers (read-only mirrors made at CMD_BUF_UPLOAD)
struct Shadow { std::vector<uint8_t> vtx; std::vector<uint16_t> idx; bool have = false; };
std::vector<Shadow> g_shadow;
size_t g_shadowBytes = 0;
uint64_t g_shadowRefused = 0;

uint32_t g_stage = 0;
bool g_stageSet = false;
uint32_t g_picId = 0;
std::chrono::steady_clock::time_point g_lastWrite;

// ---- writer thread (as the first-use list's: a snapshot is handed over, the file is written there)
struct Job { std::string path, text; };
pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
std::vector<Job>& g_queue = *new std::vector<Job>; // (never destroyed: the writer thread may still be looking at it when the process exits)
bool g_thread = false, g_threadFailed = false;
std::atomic<unsigned> g_written{0}, g_failed{0};
void* WriterMain(void*) {
    for (;;) {
        Job j;
        pthread_mutex_lock(&g_mu);
        while (g_queue.empty()) pthread_cond_wait(&g_cv, &g_mu);
        j = std::move(g_queue.front());
        g_queue.erase(g_queue.begin());
        pthread_mutex_unlock(&g_mu);
        const std::string tmp = j.path + ".tmp";
        bool ok = false;
        if (FILE* f = std::fopen(tmp.c_str(), "wb")) {
            ok = std::fwrite(j.text.data(), 1, j.text.size(), f) == j.text.size();
            ok = std::fflush(f) == 0 && ok;
            if (std::fclose(f) != 0) ok = false;
            if (ok && std::rename(tmp.c_str(), j.path.c_str()) != 0) { std::remove(j.path.c_str()); ok = std::rename(tmp.c_str(), j.path.c_str()) == 0; }
            if (!ok) std::remove(tmp.c_str());
        }
        (ok ? g_written : g_failed).fetch_add(1, std::memory_order_release);
    }
    return nullptr;
}
void Hand(std::string path, std::string text) {
    if (g_threadFailed) return;
    if (!g_thread) {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 64u << 10);
        pthread_t th;
        const int rc = pthread_create(&th, &attr, &WriterMain, nullptr);
        pthread_attr_destroy(&attr);
        if (rc != 0) { g_threadFailed = true; if (port_log_summary) port_log_summary("texmag: the writer thread could not be started (%d)", rc); return; }
        pthread_detach(th);
        g_thread = true;
    }
    pthread_mutex_lock(&g_mu);
    if (g_queue.size() >= 8) g_queue.erase(g_queue.begin());
    g_queue.push_back(Job{std::move(path), std::move(text)});
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mu);
}

void Init() {
    g_every = EnvInt("GXV_TEXMAG_CENSUS", 0);
    g_armed = g_every > 0 ? 1 : 0;
    if (!g_armed) return;
    if (vgx2_mode) {
        g_armed = 0;
        if (port_log_summary) port_log_summary("texmag: GXV_TEXMAG_CENSUS declined: GXV_FE_TWOCORE is on (the second worker's draws are not in the player's buffer)");
        return;
    }
    g_triPer = std::max<long>(1, std::min<long>(256, EnvInt("GXV_TEXMAG_TRIS", 16)));
    g_triBudget = std::max<long>(64, EnvInt("GXV_TEXMAG_BUDGET", 8192));
    g_subdiv = static_cast<int>(std::max<long>(0, std::min<long>(5, EnvInt("GXV_TEXMAG_SUBDIV", 3))));
    g_pieceArea = static_cast<double>(std::max<long>(500, EnvInt("GXV_TEXMAG_PIECE_PX", 6000)));
    g_pieceCap = static_cast<uint32_t>(std::max<long>(0, EnvInt("GXV_TEXMAG_PIECES", 4000)));
    g_minSide = EnvInt("GXV_TEXMAG_MINSIDE", 16);
    g_anyWrite = EnvInt("GXV_TEXMAG_ANYWRITE", 0) != 0;
    g_writeSec = std::max<long>(5, EnvInt("GXV_TEXMAG_WRITE_S", 60));
    g_shadowCap = static_cast<size_t>(std::max<long>(0, EnvInt("GXV_TEXMAG_SHADOW_MB", 12))) << 20;
    if (const char* s = std::getenv("GXV_TEXMAG_SCREEN")) {
        int w = 0, h = 0;
        if (std::sscanf(s, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) { g_scrW = w; g_scrH = h; }
    }
    if (const char* d = std::getenv("GXV_TEXMAG_DIR")) g_dir = d;
#if defined(__vita__) || defined(__VGX_VITA__)
    else g_dir = "ux0:data/smg/";
#endif
    if (!g_dir.empty() && g_dir.back() != '/' && g_dir.back() != ':') g_dir += '/';
    g_lastWrite = std::chrono::steady_clock::now();
    if (port_log_summary)
        port_log_summary("texmag: census on (GXV_TEXMAG_CENSUS=%ld): one picture in %ld, up to %ld triangles per draw (%ld a picture), screen %dx%d, static shadow %u MiB, files %stexmag-<stage>.txt every %ld s and at each scene's end",
                         g_every, g_every, g_triPer, g_triBudget, g_scrW, g_scrH, static_cast<unsigned>(g_shadowCap >> 20), g_dir.c_str(), g_writeSec);
}

// ---- shadows of the static buffers
void ShadowFree(uint32_t id) {
    if (id < g_shadow.size() && g_shadow[id].have) {
        g_shadowBytes -= g_shadow[id].vtx.size() + g_shadow[id].idx.size() * 2;
        g_shadow[id] = Shadow{};
    }
}
void ShadowUpdate() {
    for (const Cmd& c : g_buf.cmds) {
        if (c.type == CMD_BUF_FREE) ShadowFree(c.arg);
        else if (c.type == CMD_BUF_UPLOAD && c.arg < g_buf.bufUploads.size()) {
            const BufUpload& u = g_buf.bufUploads[c.arg];
            if (u.bufId > 65535) continue;
            ShadowFree(u.bufId);
            const size_t n = u.vtx.size() + u.idx.size() * 2;
            if (g_shadowBytes + n > g_shadowCap) { ++g_shadowRefused; continue; }
            if (u.bufId >= g_shadow.size()) g_shadow.resize(static_cast<size_t>(u.bufId) + 64);
            Shadow& s = g_shadow[u.bufId];
            s.vtx = u.vtx;
            s.idx = u.idx;
            s.have = true;
            g_shadowBytes += n;
        }
    }
}

// ---- geometry
struct PV { double x, y, z, w, a[2 * kMaxCoord]; };
struct SV { double X, Y, q, a[2 * kMaxCoord]; };
inline void Lerp(const PV& p, const PV& r, double t, PV& o, int na) {
    o.x = p.x + (r.x - p.x) * t; o.y = p.y + (r.y - p.y) * t; o.z = p.z + (r.z - p.z) * t; o.w = p.w + (r.w - p.w) * t;
    for (int i = 0; i < na; ++i) o.a[i] = p.a[i] + (r.a[i] - p.a[i]) * t;
}
int ClipHomog(const PV* in, int n, PV* out, int plane, int na) {
    int m = 0;
    for (int i = 0; i < n; ++i) {
        const PV& c = in[i];
        const PV& x = in[(i + 1) % n];
        const double dc = plane == 0 ? c.z + c.w : c.w - c.z, dn = plane == 0 ? x.z + x.w : x.w - x.z;
        if (dc >= 0) out[m++] = c;
        if ((dc >= 0) != (dn >= 0)) Lerp(c, x, dc / (dc - dn), out[m++], na);
    }
    return m;
}
// keeps side * (v - lim) >= 0 for v = X (axis 0) or Y (axis 1)
int CropScreen(const SV* in, int n, SV* out, int axis, double lim, double side, int na) {
    int m = 0;
    for (int i = 0; i < n; ++i) {
        const SV& c = in[i];
        const SV& x = in[(i + 1) % n];
        const double dc = side * ((axis ? c.Y : c.X) - lim), dn = side * ((axis ? x.Y : x.X) - lim);
        if (dc >= 0) out[m++] = c;
        if ((dc >= 0) != (dn >= 0)) {
            const double t = dc / (dc - dn);
            SV& o = out[m++];
            o.X = c.X + (x.X - c.X) * t; o.Y = c.Y + (x.Y - c.Y) * t; o.q = c.q + (x.q - c.q) * t;
            for (int k = 0; k < na; ++k) o.a[k] = c.a[k] + (x.a[k] - c.a[k]) * t;
        }
    }
    return m;
}

struct Use { // one texture stage of the draw
    int coord = 0;     // index into the draw's distinct coordinates
    uint32_t texId = 0;
    const TexInfo* ti = nullptr;
    // per picture draw accumulators
    double all = 0, ge[4] = {}, mini[3] = {}, sumMag = 0, sumLod = 0, sumLodMin = 0, maxMag = 0, minMag = 1e30;
};
struct Coord { int src = 0, tg = 0; };

struct Pic { double sx = 1, sy = 1; int rx0 = 0, ry0 = 0, rx1 = 640, ry1 = 456; double scr = 960.0 * 544.0; long k = 16; };

Acc* AccFor(const TexInfo& ti) {
    auto it = g_acc.find(ti.key);
    if (it == g_acc.end()) {
        if (g_acc.size() >= kMaxTex) return nullptr;
        Acc a;
        a.key = ti.key; a.w = ti.w; a.h = ti.h; a.fmt = ti.fmt; a.player = ti.player;
        it = g_acc.emplace(ti.key, a).first;
    }
    return &it->second;
}

void MeasureDraw(const DrawRec& d, const Pic& pic, bool inPass) {
    ++g_c.draws;
    if (d.prim != PRIM_TRIANGLES || d.idxCount < 3 || !d.texMask) return;
    ++g_c.texDraws;
    if (inPass) { ++g_c.sPass; return; }
    const RenderState& rs = d.rs;
    if ((!rs.colorWrite && !g_anyWrite) || rs.cull == CULL_ALL) { ++g_c.sDepth; return; }
    if (d.vsId >= g_vsReg.keys.size() || d.fsId >= g_fsReg.keys.size()) return;
    if (std::fabs(rs.vpW * rs.vpH) * 4.0 < static_cast<double>(pic.rx1 - pic.rx0) * (pic.ry1 - pic.ry0)) { ++g_c.sVp; return; } // (under a quarter of the picture)
    const VsKey& vs = g_vsReg.keys[d.vsId];
    const FsKey& fs = g_fsReg.keys[d.fsId];
    const VtxLayout L = MakeLayout(d.layoutMask);

    Use uses[8];
    int nu = 0;
    Coord coords[kMaxCoord];
    int nc = 0;
    for (int s = 0; s < fs.numStages && s < 16; ++s) {
        const FsKey::Stage& st = fs.stage[s];
        if (!st.texEnable || st.texOff) continue;
        if (st.ind) { ++g_c.sInd; continue; }
        if (st.texMap > 7 || !(d.texMask & (1u << st.texMap))) continue;
        const uint32_t texId = d.tex[st.texMap].texId;
        const TexInfo* ti = texId < g_texInfo.size() && g_texInfo[texId].key ? &g_texInfo[texId] : nullptr;
        if (!ti) { ++g_c.sNoTex; continue; }
        if (static_cast<long>(std::max(ti->w, ti->h)) < g_minSide || std::min(ti->w, ti->h) <= 2) { ++g_c.sTiny; continue; }
        const int c = st.texCoord;
        if (c >= vs.numTexGens || c >= 8) { ++g_c.sNoCoord; continue; }
        const VsKey::TexGen& tg = vs.tg[c];
        if (tg.type != 0 || tg.src < 5 || tg.src > 12 || tg.post == 2) { ++g_c.sEnv; continue; }
        if (tg.proj || (fs.texProjMask & (1u << c))) { ++g_c.sProj; continue; }
        if (!(d.layoutMask & (VL_TEX0 << (tg.src - 5)))) { ++g_c.sNoCoord; continue; }
        bool dup = false;
        for (int u = 0; u < nu; ++u) dup |= uses[u].texId == texId && coords[uses[u].coord].tg == c;
        if (dup || nu >= 8) continue;
        int ci = -1;
        for (int k = 0; k < nc; ++k) if (coords[k].tg == c) ci = k;
        if (ci < 0) {
            if (nc >= static_cast<int>(kMaxCoord)) continue;
            ci = nc++;
            coords[ci].src = tg.src - 5;
            coords[ci].tg = c;
        }
        uses[nu].coord = ci;
        uses[nu].texId = texId;
        uses[nu].ti = ti;
        ++nu;
    }
    if (!nu) return;

    // blocks
    const size_t un = g_buf.uni.size();
    const uint32_t pb = d.ublock[UB_PROJ], mb = d.ublock[UB_MV], tb = d.ublock[UB_TEXM], lb = d.ublock[UB_PAL];
    if (pb == kNoBlock || static_cast<size_t>(pb) + 16 > un) { ++g_c.sBlock; return; }
    const float* P = g_buf.uni.data() + pb;
    const float* M = nullptr;
    const float* PAL = nullptr;
    size_t palRows = 0;
    int palMax = 61;
    if (vs.perVtxPos) {
        if (lb == kNoBlock) { ++g_c.sBlock; return; }
        PAL = g_buf.uni.data() + lb;
        const int K = vs.perVtxPos == 3 ? 12 : 30;
        palRows = (vs.perVtxPos == 1 ? kPalRowsFull : static_cast<size_t>(K) * 2);
        palRows = std::min(palRows, (un - lb) / 4);
        palMax = vs.perVtxPos == 1 ? 61 : K - 3;
    } else {
        if (mb == kNoBlock || static_cast<size_t>(mb) + 12 > un) { ++g_c.sBlock; return; }
        M = g_buf.uni.data() + mb;
    }
    const float* TM[kMaxCoord] = {};
    const float* PTM[kMaxCoord] = {};
    bool tmi[kMaxCoord] = {};
    for (int k = 0; k < nc; ++k) {
        const int c = coords[k].tg;
        const VsKey::TexGen& tg = vs.tg[c];
        if (tg.perVtxMtx) {
            if (!(d.layoutMask & VL_TMI) || !PAL) { nu = 0; ++g_c.sBlock; break; }
            tmi[k] = true;
        } else {
            if (tb == kNoBlock || static_cast<size_t>(tb) + static_cast<size_t>(c) * 12 + 12 > un) { nu = 0; ++g_c.sBlock; break; }
            TM[k] = g_buf.uni.data() + tb + c * 12;
        }
        if (tg.post) {
            if (tb == kNoBlock || static_cast<size_t>(tb) + 96 + static_cast<size_t>(c) * 12 + 12 > un) { nu = 0; ++g_c.sBlock; break; }
            PTM[k] = g_buf.uni.data() + tb + 96 + c * 12;
        }
    }
    if (!nu) return;

    // vertex source
    const uint8_t* vtx;
    size_t vtxSize;
    const uint16_t* idx;
    size_t base = d.vtxOffset;
    if (d.vbuf) {
        if (d.vbuf >= g_shadow.size() || !g_shadow[d.vbuf].have) { ++g_c.sNoStatic; return; }
        const Shadow& sh = g_shadow[d.vbuf];
        if (static_cast<size_t>(d.idxOffset) + d.idxCount > sh.idx.size()) { ++g_c.sNoStatic; return; }
        vtx = sh.vtx.data(); vtxSize = sh.vtx.size(); idx = sh.idx.data() + d.idxOffset;
    } else {
        if (static_cast<size_t>(d.idxOffset) + d.idxCount > g_buf.idx.size()) { ++g_c.sBlock; return; }
        if (!g_buf.vtxChunks.empty()) {
            if (d.vchunk >= g_buf.vtxChunks.size()) { ++g_c.sBlock; return; }
            base += g_buf.vtxChunks[d.vchunk];
        }
        vtx = g_buf.vtx.data(); vtxSize = g_buf.vtx.size(); idx = g_buf.idx.data() + d.idxOffset;
    }

    const uint32_t ntri = d.idxCount / 3;
    const uint32_t nsamp = std::min<uint32_t>(ntri, static_cast<uint32_t>(pic.k));
    const int na = 2 * nc;
    uint32_t valid = 0;
    // scissor / picture rectangle (EFB pixels)
    const double rx0 = std::max<double>(pic.rx0, std::max(0, static_cast<int>(rs.scX)));
    const double ry0 = std::max<double>(pic.ry0, std::max(0, static_cast<int>(rs.scY)));
    const double rx1 = std::min<double>(pic.rx1, static_cast<double>(rs.scX) + rs.scW);
    const double ry1 = std::min<double>(pic.ry1, static_cast<double>(rs.scY) + rs.scH);
    const bool flip = (rs.vpW < 0) != (rs.vpH < 0);

    auto fetch = [&](uint16_t vi, PV& o) {
        const size_t off = base + static_cast<size_t>(vi) * L.stride;
        if (off > vtxSize || vtxSize - off < L.stride) return false;
        const uint8_t* v = vtx + off;
        double p[4];
        if (d.layoutMask & VL_QPOS) { int16_t q[4]; std::memcpy(q, v, 8); for (int i = 0; i < 4; ++i) p[i] = q[i]; }
        else { float f[4]; std::memcpy(f, v, 16); for (int i = 0; i < 4; ++i) p[i] = f[i]; }
        double e[3];
        if (PAL) {
            int pi = static_cast<int>(std::floor(p[3] + 0.5));
            pi = pi < 0 ? 0 : (pi > palMax ? palMax : pi);
            if (static_cast<size_t>(pi) + 3 > palRows) return false;
            for (int r = 0; r < 3; ++r) { const float* m = PAL + (pi + r) * 4; e[r] = m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3]; }
        } else {
            for (int r = 0; r < 3; ++r) e[r] = M[r * 4] * p[0] + M[r * 4 + 1] * p[1] + M[r * 4 + 2] * p[2] + M[r * 4 + 3];
        }
        o.x = P[0] * e[0] + P[4] * e[1] + P[8] * e[2] + P[12];
        o.y = P[1] * e[0] + P[5] * e[1] + P[9] * e[2] + P[13];
        o.z = P[2] * e[0] + P[6] * e[1] + P[10] * e[2] + P[14];
        o.w = P[3] * e[0] + P[7] * e[1] + P[11] * e[2] + P[15];
        if (!std::isfinite(o.x) || !std::isfinite(o.y) || !std::isfinite(o.z) || !std::isfinite(o.w)) return false;
        for (int k = 0; k < nc; ++k) {
            double uv[2];
            const uint32_t to = L.offTex[coords[k].src];
            if ((d.layoutMask & VL_QTEX) && coords[k].src < 2) { int16_t q[2]; std::memcpy(q, v + to, 4); uv[0] = q[0]; uv[1] = q[1]; }
            else { float f[2]; std::memcpy(f, v + to, 8); uv[0] = f[0]; uv[1] = f[1]; }
            double s, t;
            if (tmi[k]) {
                int ti = v[L.offTmi + coords[k].tg];
                ti = ti > 61 ? 61 : ti;
                if (static_cast<size_t>(ti) + 2 > palRows) return false;
                const float* r0 = PAL + ti * 4;
                const float* r1 = r0 + 4;
                s = r0[0] * uv[0] + r0[1] * uv[1] + r0[2] + r0[3];
                t = r1[0] * uv[0] + r1[1] * uv[1] + r1[2] + r1[3];
            } else {
                const float* m = TM[k];
                s = m[0] * uv[0] + m[1] * uv[1] + m[2] + m[3];
                t = m[4] * uv[0] + m[5] * uv[1] + m[6] + m[7];
            }
            if (PTM[k]) {
                const float* m = PTM[k];
                const double s2 = m[0] * s + m[1] * t + m[2] + m[3], t2 = m[4] * s + m[5] * t + m[6] + m[7];
                s = s2; t = t2;
            }
            if (!std::isfinite(s) || !std::isfinite(t)) return false;
            o.a[2 * k] = s;
            o.a[2 * k + 1] = t;
        }
        return true;
    };

    // one triangle (clip space with its coordinates): subdivided while it is large on the screen (the magnification varies across a
    // tilted plane; a bin is decided per piece), then clipped, cropped to the picture and measured
    std::function<void(const PV*, int)> piece = [&](const PV* tri, int depth) {
        if (depth < g_subdiv && g_pieces < g_pieceCap) {
            bool allIn = true;
            for (int i = 0; i < 3; ++i) allIn &= tri[i].w > 1e-9;
            double big = 1e30;
            if (allIn) {
                double sg2 = 0;
                for (int i = 0; i < 3; ++i) {
                    const PV& p = tri[i]; const PV& r = tri[(i + 1) % 3];
                    sg2 += (p.x / p.w) * (r.y / r.w) - (r.x / r.w) * (p.y / p.w);
                }
                big = std::fabs(sg2) * 0.25 * std::fabs(rs.vpW * rs.vpH) * pic.sx * pic.sy; // (NDC area 4 is the viewport)
            }
            if (big > g_pieceArea) {
                PV m[3], q[3];
                for (int i = 0; i < 3; ++i) Lerp(tri[i], tri[(i + 1) % 3], 0.5, m[i], na); // m[i]: midpoint of edge i -> i+1
                const PV t0[3] = {tri[0], m[0], m[2]}, t1[3] = {m[0], tri[1], m[1]}, t2[3] = {m[2], m[1], tri[2]}, t3[3] = {m[0], m[1], m[2]};
                (void)q;
                ++g_pieces;
                piece(t0, depth + 1); piece(t1, depth + 1); piece(t2, depth + 1); piece(t3, depth + 1);
                return;
            }
        }
        PV a[12], b[12];
        int n = 3;
        for (int i = 0; i < 3; ++i) a[i] = tri[i];
        bool inside = true;
        for (int i = 0; i < 3; ++i) inside &= tri[i].w > 1e-9 && tri[i].z >= -tri[i].w && tri[i].z <= tri[i].w;
        if (!inside) {
            n = ClipHomog(a, n, b, 0, na);
            if (n >= 3) n = ClipHomog(b, n, a, 1, na);
            if (n < 3) { ++g_c.trisOff; return; }
        }
        SV s0[16], s1[16];
        for (int i = 0; i < n; ++i) {
            const double w = a[i].w;
            if (!(w > 1e-12)) { n = 0; break; }
            s0[i].X = rs.vpX + (a[i].x / w + 1.0) * 0.5 * rs.vpW;
            s0[i].Y = rs.vpY + (a[i].y / w + 1.0) * 0.5 * rs.vpH;
            s0[i].q = 1.0 / w;
            for (int k = 0; k < na; ++k) s0[i].a[k] = a[i].a[k] / w;
        }
        if (n < 3) { ++g_c.trisOff; return; }
        n = CropScreen(s0, n, s1, 0, rx0, 1.0, na);
        if (n >= 3) n = CropScreen(s1, n, s0, 0, rx1, -1.0, na); else n = 0;
        if (n >= 3) n = CropScreen(s0, n, s1, 1, ry0, 1.0, na); else n = 0;
        if (n >= 3) n = CropScreen(s1, n, s0, 1, ry1, -1.0, na); else n = 0;
        if (n < 3) { ++g_c.trisOff; return; }
        double sg = 0;
        for (int i = 0; i < n; ++i) { const SV& p = s0[i]; const SV& r = s0[(i + 1) % n]; sg += p.X * r.Y - r.X * p.Y; }
        sg *= 0.5;
        if (flip) sg = -sg;
        if (sg == 0 || (rs.cull == CULL_BACK && sg < 0) || (rs.cull == CULL_FRONT && sg > 0)) { ++g_c.trisCulled; return; }
        const double apx = std::fabs(sg) * pic.sx * pic.sy;
        for (int u = 0; u < nu; ++u) {
            Use& us = uses[u];
            const int k = us.coord;
            double ta = 0;
            for (int i = 0; i < n; ++i) {
                const SV& p = s0[i]; const SV& r = s0[(i + 1) % n];
                const double ps = p.a[2 * k] / p.q * us.ti->w, pt = p.a[2 * k + 1] / p.q * us.ti->h;
                const double rs_ = r.a[2 * k] / r.q * us.ti->w, rt = r.a[2 * k + 1] / r.q * us.ti->h;
                ta += ps * rt - rs_ * pt;
            }
            ta = std::fabs(ta) * 0.5;
            if (!(ta > 1e-9)) { ++g_c.sDegen; continue; }
            double m = std::sqrt(apx / ta);
            if (m > 1000.0) m = 1000.0;
            const double est = apx; // (scaled by ntri / valid below)
            us.all += est;
            for (int bi = 0; bi < 4; ++bi) if (m >= (bi == 0 ? 1.0 : bi == 1 ? 1.5 : bi == 2 ? 2.0 : 3.0)) us.ge[bi] += est;
            const double lod = -std::log2(m);
            for (int bi = 0; bi < 3; ++bi) if (lod >= (bi == 0 ? 0.5 : bi == 1 ? 1.0 : 2.0)) us.mini[bi] += est;
            us.sumMag += est * m;
            us.sumLod += est * lod;
            if (lod > 0) us.sumLodMin += est * lod;
            if (m > us.maxMag) us.maxMag = m;
            if (m < us.minMag) us.minMag = m;
        }
    };
    for (uint32_t j = 0; j < nsamp; ++j) {
        const uint32_t t = nsamp == ntri ? j : static_cast<uint32_t>((static_cast<uint64_t>(j) * 2 + 1) * ntri / (2ull * nsamp));
        PV tri[3];
        ++g_c.tris;
        if (!fetch(idx[t * 3], tri[0]) || !fetch(idx[t * 3 + 1], tri[1]) || !fetch(idx[t * 3 + 2], tri[2])) { ++g_c.trisBad; continue; }
        ++valid;
        piece(tri, 0);
    }
    if (!valid) return;
    ++g_c.measured;
    const double scale = static_cast<double>(ntri) / valid;
    for (int u = 0; u < nu; ++u) {
        Use& us = uses[u];
        if (us.all <= 0 && us.maxMag <= 0) continue;
        Acc* a = AccFor(*us.ti);
        if (!a) { ++g_c.sCapped; continue; }
        if (a->curPic != g_picId) { a->curPic = g_picId; a->curAll = a->curPx15 = 0; g_touched.push_back(a); }
        a->px[0] += us.all * scale;
        for (int bi = 0; bi < 4; ++bi) a->px[bi + 1] += us.ge[bi] * scale;
        for (int bi = 0; bi < 3; ++bi) a->mini[bi] += us.mini[bi] * scale;
        a->sumMag += us.sumMag * scale;
        a->sumLod += us.sumLod * scale;
        a->sumLodMin += us.sumLodMin * scale;
        if (us.maxMag > a->maxMag) a->maxMag = us.maxMag;
        if (us.minMag < a->minMag) a->minMag = us.minMag;
        a->curAll += us.all * scale;
        a->curPx15 += us.ge[1] * scale;
        g_c.totalPx += us.all * scale;
    }
}

const char* FmtName(unsigned f) {
    static const char* const n[16] = {"I4", "I8", "IA4", "IA8", "RGB565", "RGB5A3", "RGBA8", "?7", "C4", "C8", "C14X2", "?B", "?C", "?D", "CMPR", "?F"};
    return n[f & 15];
}

void FoldPicture() {
    for (Acc* a : g_touched) {
        ++a->pics;
        a->peakPx15 = std::max(a->peakPx15, a->curPx15);
    }
    g_touched.clear();
}

// the file text and the log lines for the stage's accumulation so far; `log`: also the basic-log lines
std::string Snapshot(uint32_t stage, bool log, const char* why) {
    std::vector<const Acc*> v;
    v.reserve(g_acc.size());
    for (const auto& kv : g_acc) v.push_back(&kv.second);
    std::sort(v.begin(), v.end(), [](const Acc* a, const Acc* b) { return a->px[2] != b->px[2] ? a->px[2] > b->px[2] : a->key < b->key; });
    const double P = static_cast<double>(g_c.sampled ? g_c.sampled : 1), scr = static_cast<double>(g_scrW) * g_scrH;
    char line[1024];
    std::string out;
    out.reserve(64 << 10);
    std::snprintf(line, sizeof line,
                  "# texmag 2\nstage=%08x\nreason=%s\nscreen=%dx%d\nevery=%ld\ntris_per_draw=%ld\npictures_seen=%llu\npictures_sampled=%llu\n"
                  "draws=%llu\ntextured_draws=%llu\nmeasured_draws=%llu\ntriangles_sampled=%llu\ntriangles_culled=%llu\ntriangles_off_screen=%llu\ntriangles_bad=%llu\n"
                  "skip_pass_draws=%llu\nskip_small_viewport=%llu\nskip_no_colour_write=%llu\nskip_no_guest_texture=%llu\nskip_tiny_texture=%llu\n"
                  "skip_env_texgen=%llu\nskip_projective=%llu\nskip_indirect=%llu\nskip_no_coord=%llu\nskip_static_not_shadowed=%llu\nskip_block=%llu\nskip_degenerate_uv=%llu\n"
                  "skip_table_full=%llu\nshadow_bytes=%llu\nshadow_refused=%llu\ntextured_px_per_picture=%.0f\noverdraw_textured=%.3f\ncensus_ms_per_sampled_picture=%.3f\n"
                  "# cols: key w h fmt player pics px_all px_ge1 px_ge15 px_ge2 px_ge3 px_lod05 px_lod1 px_lod2 mean_mag mean_lod mean_lod_min max_mag min_mag peak_px15 peak_share_pct avg_share_pct\n"
                  "# (pixel columns are display pixels summed over the sampled pictures; divide by pictures_sampled for a picture's mean; lod = log2(texels per pixel), < 0 magnified)\n",
                  stage, why, g_scrW, g_scrH, g_every, g_triPer, (unsigned long long)g_c.seen, (unsigned long long)g_c.sampled, (unsigned long long)g_c.draws,
                  (unsigned long long)g_c.texDraws, (unsigned long long)g_c.measured, (unsigned long long)g_c.tris, (unsigned long long)g_c.trisCulled,
                  (unsigned long long)g_c.trisOff, (unsigned long long)g_c.trisBad, (unsigned long long)g_c.sPass, (unsigned long long)g_c.sVp, (unsigned long long)g_c.sDepth,
                  (unsigned long long)g_c.sNoTex, (unsigned long long)g_c.sTiny, (unsigned long long)g_c.sEnv, (unsigned long long)g_c.sProj, (unsigned long long)g_c.sInd,
                  (unsigned long long)g_c.sNoCoord, (unsigned long long)g_c.sNoStatic, (unsigned long long)g_c.sBlock, (unsigned long long)g_c.sDegen,
                  (unsigned long long)g_c.sCapped, (unsigned long long)g_shadowBytes, (unsigned long long)g_shadowRefused, g_c.totalPx / P, g_c.totalPx / P / scr,
                  g_c.sampled ? g_c.ns / 1e6 / P : 0.0);
    out += line;
    uint32_t lines = 0;
    for (const Acc* a : v) {
        if (lines >= kFileLines && !a->player) continue;
        ++lines;
        const double A = a->px[0] > 0 ? a->px[0] : 1;
        std::snprintf(line, sizeof line, "%016llx\t%u\t%u\t%s\t%u\t%u\t%.0f\t%.0f\t%.0f\t%.0f\t%.0f\t%.0f\t%.0f\t%.0f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.0f\t%.1f\t%.2f\n",
                      (unsigned long long)a->key, a->w, a->h, FmtName(a->fmt), a->player, a->pics, a->px[0], a->px[1], a->px[2], a->px[3], a->px[4], a->mini[0], a->mini[1],
                      a->mini[2], a->sumMag / A, a->sumLod / A, a->sumLodMin / A, a->maxMag, a->minMag > 1e29 ? 0.0 : a->minMag, a->peakPx15, 100.0 * a->peakShare,
                      100.0 * a->px[0] / (P * scr));
        out += line;
    }
    if (log && port_log_summary) {
        port_log_summary("texmag: stage %08x (%s): %llu pictures sampled of %llu (1 in %ld), %llu draws, %llu textured, %llu measured, %llu triangles (%llu culled, %llu off screen); "
                         "draws skipped: no guest texture %llu, tiny %llu, environment %llu, projective %llu, indirect %llu; static draws not shadowed %llu; textured pixels %.2f x the screen; census %.2f ms a sampled picture (max %.2f)",
                         stage, why, (unsigned long long)g_c.sampled, (unsigned long long)g_c.seen, g_every, (unsigned long long)g_c.draws, (unsigned long long)g_c.texDraws,
                         (unsigned long long)g_c.measured, (unsigned long long)g_c.tris, (unsigned long long)g_c.trisCulled, (unsigned long long)g_c.trisOff,
                         (unsigned long long)g_c.sNoTex, (unsigned long long)g_c.sTiny, (unsigned long long)g_c.sEnv, (unsigned long long)g_c.sProj, (unsigned long long)g_c.sInd,
                         (unsigned long long)g_c.sNoStatic, g_c.totalPx / P / scr, g_c.sampled ? g_c.ns / 1e6 / P : 0.0, g_c.nsMax / 1e6);
        auto one = [&](const char* tag, unsigned rank, const Acc* a) {
            const double A = a->px[0] > 0 ? a->px[0] : 1;
            port_log_summary("texmag: stage %08x %s%u key %016llx %ux%u %s pics %u/%llu px15 %.0f/pic (peak %.0f) ge1 %.0f ge2 %.0f ge3 %.0f minified lod>=0.5 %.0f lod>=1 %.0f lod>=2 %.0f; "
                             "mean mag %.2f mean lod %+.2f (minified part %.2f) max %.2f min %.3f; share %.2f%% (peak %.1f%%)",
                             stage, tag, rank, (unsigned long long)a->key, a->w, a->h, FmtName(a->fmt), a->pics, (unsigned long long)g_c.sampled, a->px[2] / P, a->peakPx15,
                             a->px[1] / P, a->px[3] / P, a->px[4] / P, a->mini[0] / P, a->mini[1] / P, a->mini[2] / P, a->sumMag / A, a->sumLod / A, a->sumLodMin / A, a->maxMag,
                             a->minMag > 1e29 ? 0.0 : a->minMag, 100.0 * a->px[0] / (P * scr), 100.0 * a->peakShare);
        };
        unsigned r = 0;
        for (const Acc* a : v) { if (++r > kLogLines) break; if (a->px[2] <= 0) break; one("top", r, a); }
        r = 0;
        for (const Acc* a : v) if (a->player) one("player ", ++r, a); // (every player texture seen: Mario's and Luigi's, by the HD pack's list)
    }
    return out;
}

void Write(uint32_t stage, const char* why, bool log) {
    if (g_acc.empty() && !g_c.sampled) return; // (a stage with sampled pictures but no texture still writes its counters)
    char name[64];
    std::snprintf(name, sizeof name, "texmag-%08x.txt", stage);
    Hand(g_dir + name, Snapshot(stage, log, why));
    g_lastWrite = std::chrono::steady_clock::now();
}
void Reset() {
    g_acc.clear();
    g_touched.clear();
    const uint64_t shadowRefused = g_shadowRefused; // (the shadows stay: they belong to the buffers, not to the stage)
    g_c = Ctr{};
    g_shadowRefused = shadowRefused;
}

void Sample() {
    const auto t0 = std::chrono::steady_clock::now();
    Pic pic;
    EfbRect disp{0, 0, 0, 0};
    for (const Cmd& c : g_buf.cmds)
        if (c.type == CMD_COPY_DISP && c.arg < g_buf.copyDisp.size()) disp = g_buf.copyDisp[c.arg].src;
    if (disp.w <= 0 || disp.h <= 0) disp = EfbRect{0, 0, static_cast<int16_t>(g.efbW), static_cast<int16_t>(g.efbH)};
    pic.rx0 = disp.x; pic.ry0 = disp.y; pic.rx1 = disp.x + disp.w; pic.ry1 = disp.y + disp.h;
    pic.sx = static_cast<double>(g_scrW) / disp.w;
    pic.sy = static_cast<double>(g_scrH) / disp.h;
    pic.scr = static_cast<double>(g_scrW) * g_scrH;
    uint32_t elig = 0;
    for (const DrawRec& d : g_buf.draws) elig += d.prim == PRIM_TRIANGLES && d.texMask && d.idxCount >= 3;
    pic.k = elig ? std::max<long>(3, std::min<long>(g_triPer, g_triBudget / elig)) : g_triPer;
    ++g_picId;
    g_pieces = 0;
    bool inPass = false;
    for (const Cmd& c : g_buf.cmds) {
        if (c.type == CMD_DRAW) { if (c.arg < g_buf.draws.size()) MeasureDraw(g_buf.draws[c.arg], pic, inPass); }
        else if (c.type == CMD_PASS_BEGIN) inPass = true;
        else if (c.type == CMD_COPY_TEX && c.arg < g_buf.copyTex.size() && g_buf.copyTex[c.arg].fromPass) inPass = false;
    }
    for (Acc* a : g_touched) a->peakShare = std::max(a->peakShare, a->curAll / pic.scr);
    FoldPicture();
    ++g_c.sampled;
    const uint64_t ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    g_c.ns += ns;
    g_c.nsMax = std::max(g_c.nsMax, ns);
}

} // namespace texmag

bool TexMagEnabled() {
    if (texmag::g_armed < 0) texmag::Init();
    return texmag::g_armed > 0;
}
void TexMagNoteTexture(uint32_t texId, uint64_t key, uint32_t w, uint32_t h, uint32_t fmt) {
    if (texmag::g_armed <= 0 || texId > (1u << 20) || w > 65535 || h > 65535) return;
    if (texId >= texmag::g_texInfo.size()) texmag::g_texInfo.resize(static_cast<size_t>(texId) + 512);
    texmag::TexInfo& t = texmag::g_texInfo[texId];
    t.key = key ? key : 1;
    t.w = static_cast<uint16_t>(w);
    t.h = static_cast<uint16_t>(h);
    t.fmt = static_cast<uint8_t>(fmt);
    t.player = IsPlayerTexture(texId) ? 1 : 0;
}
void TexMagSceneEnd() {
    if (texmag::g_armed <= 0 || vgx2_mode) return;
    texmag::Write(texmag::g_stage, "scene end", true);
}
void TexMagFrame() {
    using namespace texmag;
    if (__builtin_expect(g_armed == 0, 1)) return;
    if (g_armed < 0) Init();
    if (g_armed <= 0) return;
    if (vgx2_mode) return;
    ShadowUpdate();
    ++g_c.seen;
    const uint32_t key = port_scene_key ? port_scene_key() : 0;
    if (!g_stageSet) { g_stage = key; g_stageSet = true; }
    else if (key != g_stage) {
        Write(g_stage, "stage changed", true);
        Reset();
        g_stage = key;
    }
    if (g_c.seen % static_cast<uint64_t>(g_every) == static_cast<uint64_t>(g_every) / 2) Sample();
    if (std::chrono::steady_clock::now() - g_lastWrite >= std::chrono::seconds(g_writeSec)) Write(g_stage, "periodic", true);
}
#endif
