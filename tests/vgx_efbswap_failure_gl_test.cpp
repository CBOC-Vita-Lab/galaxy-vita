// Real GLES regression for a deleted EFB-rotation name left in the binding cache.
// Mesa does not immediately reuse that name, so only glGenTextures name selection
// is replaced. Deletion, binding, texture storage, rendering and pixel reads stay real.
#include "vgx_cmd.h"
#include <GLES2/gl2.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace vgx { bool LogEnabled() { return false; } }
namespace {
bool reuseNextDeletion = false;
GLuint recycled = 0;
unsigned reused = 0;
bool inspectNextStorage = false;
GLuint storageName = 0;
GLint storageBinding = -1;
bool deletionWasBound = false;
bool deletionUnbound = false;
int failures = 0;
unsigned glErrors = 0;

void Check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++failures;
}

void Copy(unsigned frameNumber, unsigned texId) {
    vgx::CmdBuf frame;
    frame.frame = frameNumber;
    vgx::CopyTexCmd copy;
    copy.texId = texId;
    copy.src = vgx::EfbRect{0, 0, 640, 456};
    copy.dstW = 640;
    copy.dstH = 456;
    for (int i = 0; i < 4; ++i) copy.mat[i][i] = 1;
    frame.copyTex.push_back(copy);
    frame.cmds.push_back(vgx::Cmd{vgx::CMD_COPY_TEX, 0});
    vgx::backend::Execute(frame);
}

bool ReadRed(unsigned texId, std::vector<uint8_t>& rgba) {
    int w = 0, h = 0;
    const bool readable = vgx::backend::DebugReadTexture(texId, rgba, w, h);
    return readable && w == 640 && h == 456 &&
        rgba[(static_cast<size_t>(2) * w + 2) * 4] >= 250 &&
        rgba[(static_cast<size_t>(2) * w + 2) * 4 + 1] <= 5 &&
        rgba[(static_cast<size_t>(2) * w + 2) * 4 + 2] <= 5 &&
        rgba[(static_cast<size_t>(2) * w + 2) * 4 + 3] >= 250;
}
}
extern "C" void __real_glGenTextures(GLsizei, GLuint*);
extern "C" void __real_glDeleteTextures(GLsizei, const GLuint*);
extern "C" void __real_glTexImage2D(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
extern "C" GLenum __real_glGetError();
extern "C" GLenum __wrap_glGetError() {
    const GLenum error = __real_glGetError();
    if (error != GL_NO_ERROR) ++glErrors;
    return error;
}
extern "C" void __wrap_glGenTextures(GLsizei count, GLuint* textures) {
    if (recycled && count == 1) {
        *textures = recycled;
        storageName = recycled;
        inspectNextStorage = true;
        recycled = 0;
        ++reused;
        return;
    }
    __real_glGenTextures(count, textures);
}
extern "C" void __wrap_glDeleteTextures(GLsizei count, const GLuint* textures) {
    const bool armed = reuseNextDeletion && count == 1;
    if (armed) {
        GLint binding = -1;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
        deletionWasBound = binding == static_cast<GLint>(*textures);
        recycled = *textures;
        reuseNextDeletion = false;
    }
    __real_glDeleteTextures(count, textures);
    if (armed) {
        GLint binding = -1;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
        deletionUnbound = binding == 0;
    }
}
extern "C" void __wrap_glTexImage2D(GLenum target, GLint level, GLint internalFormat, GLsizei width,
                                   GLsizei height, GLint border, GLenum format, GLenum type, const void* data) {
    if (inspectNextStorage) {
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &storageBinding);
        inspectNextStorage = false;
    }
    __real_glTexImage2D(target, level, internalFormat, width, height, border, format, type, data);
}

int main(int argc, char** argv) {
    using namespace vgx;
    const bool legacy = argc > 1 && std::strcmp(argv[1], "--expect-legacy") == 0;
    const bool flip = argc > 2 && std::strcmp(argv[2], "--flip-after-first") == 0;
    if (!host::CreateContext() || !backend::Init(640, 456)) return 2;
    CmdBuf setup;
    setup.targets.push_back(SetTargetCmd{640, 456});
    setup.cmds.push_back(Cmd{CMD_SET_TARGET, 0});
    backend::Execute(setup);
    const float red[4] = {1, 0, 0, 1};
    backend::DebugDrawEfbTriangle(red);
    unsigned swaps = backend::DebugEfbSwaps();
    Copy(1, 10);
    std::vector<uint8_t> normal;
    Check(ReadRed(10, normal), "normal identity copy contains the drawn red pixel");
    Check(backend::DebugEfbSwaps() == swaps + 1, "normal identity copy rotates EFB storage");
    Check(glGetError() == GL_NO_ERROR && glErrors == 0, "normal identity copy has no GLES errors");
    uint64_t hash = 14695981039346656037ull;
    for (uint8_t byte : normal) hash = (hash ^ byte) * 1099511628211ull;
    std::printf("normal_pixels_fnv1a64=%016llx\n", static_cast<unsigned long long>(hash));

    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        backend::DebugDrawEfbTriangle(red);
        while (glGetError() != GL_NO_ERROR) {}
        const unsigned errorsBefore = glErrors;
        backend::DebugFailAllocation(1);
        reuseNextDeletion = true;
        deletionWasBound = deletionUnbound = false;
        storageBinding = -1;
        swaps = backend::DebugEfbSwaps();
        Copy(2 + attempt * 2, 20 + attempt * 2);
        Check(backend::DebugEfbSwaps() == swaps, "failed EFB allocation uses the ordinary copy path");
        Check(reused == attempt + 1 && deletionWasBound && deletionUnbound,
              "fixture recycled the just-deleted bound name after real GLES unbound it");
        std::vector<uint8_t> fallback;
        const bool redPixel = ReadRed(20 + attempt * 2, fallback);
        std::printf("attempt=%u reused_name=%u storage_binding=%d red=%d legacy=%d\n",
                    attempt + 1, storageName, storageBinding, redPixel, legacy);
        Check(legacy ? storageBinding == 0 : storageBinding == static_cast<GLint>(storageName),
              "fallback storage is allocated on the expected texture binding");
        // An incomplete legacy attachment can leave a previous pooled texture bound,
        // so a seemingly correct pixel read alone cannot establish a successful copy.
        if (!legacy) Check(redPixel && fallback == normal, "guarded fallback preserves every normal capture pixel");
        while (glGetError() != GL_NO_ERROR) {}
        Check(legacy ? glErrors > errorsBefore : glErrors == errorsBefore,
              legacy ? "legacy copy generates GLES errors" : "guarded copy generates no GLES errors, including backend-consumed errors");

        if (attempt == 0 && flip) setenv("GXV_EFB_REBIND_GUARD", legacy ? "1" : "0", 1);
        backend::DebugFailAllocation(0);
        const unsigned recoveryErrors = glErrors;
        Copy(3 + attempt * 2, 21 + attempt * 2);
        std::vector<uint8_t> recovered;
        Check(ReadRed(21 + attempt * 2, recovered) && recovered == normal,
              "next ordinary EFB rotation recovers with identical normal pixels");
        Check(backend::DebugEfbSwaps() == swaps + 1, "successful rotation resumes after allocation failure");
        Check(glGetError() == GL_NO_ERROR && glErrors == recoveryErrors, "recovery has no GLES errors");
    }
    std::printf("%s: EFB failed-allocation rebind (%s%s)\n", failures ? "FAIL" : "PASS",
                legacy ? "legacy" : "guarded", flip ? ", read-once" : "");
    return failures ? 1 : 0;
}
