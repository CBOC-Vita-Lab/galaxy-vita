// Extracts the real text of vitaGL's checked scene entry (scene_reset, scene_begin_failed), depth/stencil creation, the flush/finish
// callers and a few submitting callers from a patched vitaGL tree (patches 0009 and 0010 applied, with or without gpu_timers.py and
// fbo_msaa.py) into a host fixture include. It also audits the whole source tree: every scene_reset call and every depth
// creation call must use its result, so a new unchecked caller fails the test.
const fs = require('node:fs');
const path = require('node:path');
const [sourceRoot, output] = process.argv.slice(2);
if (!sourceRoot || !output) throw new Error('usage: preparer source-root output');
const dir = fs.existsSync(path.join(sourceRoot, 'gxm.c')) ? sourceRoot : path.join(sourceRoot, 'source');
const read = (f) => fs.readFileSync(path.join(dir, f), 'utf8');
const files = {};
for (const f of fs.readdirSync(dir)) if (f.endsWith('.c')) files[f] = read(f);

function matchBrace(text, open) {
    let depth = 1, end = open + 1;
    for (; end < text.length && depth; ++end) {
        if (text[end] === '{') ++depth;
        else if (text[end] === '}') --depth;
    }
    if (depth) throw new Error('unbalanced braces');
    return end;
}
function functionText(file, headerRegex, label) {
    const text = files[file];
    const match = new RegExp(headerRegex).exec(text);
    if (!match) throw new Error('patched ' + file + ' lacks ' + label + '; apply patches 0009 and 0010 first');
    return text.slice(match.index, matchBrace(text, match.index + match[0].length - 1));
}
function fail(why) { throw new Error('audit: ' + why); }

// ---- audit 1: scene_reset is declared to return a result and every call uses it
const shared = read('shared.h');
if (!/GLboolean scene_reset\(void\);/.test(shared)) fail('shared.h does not declare GLboolean scene_reset(void)');
let sceneResetCalls = 0;
for (const [name, text] of Object.entries(files)) {
    if (name === 'splashscreen.c') continue;
    const re = /[^\n]*\bscene_reset\(\)[^\n]*/g;
    let m;
    while ((m = re.exec(text))) {
        const line = m[0];
        if (/^GLboolean scene_reset\(void\)/.test(line)) continue;
        ++sceneResetCalls;
        // allowed: if (!scene_reset()), if (scene_reset() || ...), x = scene_reset();
        if (!/\bif \(!scene_reset\(\)\)/.test(line) && !/\bif \(scene_reset\(\) \|\|/.test(line) &&
            !/\b[A-Za-z_]+ = scene_reset\(\);/.test(line) && !/\bGLboolean [A-Za-z_]+ = scene_reset\(\);/.test(line))
            fail('unchecked scene_reset call in ' + name + ': ' + line.trim());
    }
}
if (sceneResetCalls < 17) fail('expected at least 17 scene_reset callers, found ' + sceneResetCalls);

// ---- audit 2: every depth/stencil creation call uses its result
for (const [name, text] of Object.entries(files)) {
    const re = /[^\n]*\binit_depth_stencil_buffer(_ms)?\(([^\n]*)/g;
    let m;
    while ((m = re.exec(text))) {
        const line = m[0];
        if (/^(GLboolean )?init_depth_stencil_buffer/.test(line)) continue; // definitions
        if (!/\bif \(!init_depth_stencil_buffer(_ms)?\(/.test(line) && !/\breturn init_depth_stencil_buffer_ms\(/.test(line))
            fail('unchecked depth creation in ' + name + ': ' + line.trim());
    }
}

// ---- audit 3: every sceGxmBeginScene result inside scene_reset is checked
const resetText = functionText('gxm.c', 'GLboolean scene_reset\\(void\\) \\{', 'scene_reset');
const begins = (resetText.match(/= sceGxmBeginScene\(/g) || []).length;
const beginChecks = (resetText.match(/if \(r\)\n\t+return scene_begin_failed\(\);/g) || []).length;
// display, and the framebuffer object (two textual forms of the same call, shared or own render targets, with one check after both)
if (begins < 2 || beginChecks !== 2) fail('scene_reset must check both sceGxmBeginScene calls (found ' + begins + ' calls, ' + beginChecks + ' checks)');
if (!/if \(!active_write_fb->target\)\n\t+return scene_begin_failed\(\);/.test(resetText)) fail('scene_reset does not check the render target');
if (!/if \(!init_depth_stencil_buffer(_ms)?\([^\n]*\)\)\n\t+return scene_begin_failed\(\);/.test(resetText)) fail('scene_reset does not check the depth creation');
// no failure return leaves a half-published state: the success marker comes after every begin
if (resetText.indexOf('scene_fail_streak = 0;') < resetText.lastIndexOf('return scene_begin_failed();')) fail('success marker precedes a failure return');

// ---- audit 4: glBegin keeps the result and glEnd leaves before it submits anything
const beginFn = functionText('ffp.c', 'void glBegin\\(GLenum mode\\) \\{', 'glBegin');
if (!/ffp_scene_began = scene_reset\(\);/.test(beginFn)) fail('glBegin does not keep the scene_reset result');
const endFn = functionText('ffp.c', 'void glEnd\\(void\\) \\{', 'glEnd');
const guard = endFn.indexOf('if (!ffp_scene_began) {');
const firstGxm = endFn.search(/sceGxm|gl_primitive_to_gxm|reload_ffp_shaders/);
if (guard < 0 || firstGxm < 0 || guard > firstGxm) fail('glEnd does not drop the vertices before its first gxm call');

// ---- audit 5: the query readers never wait on a fence a missing scene end cannot advance
const bufText = files['buffers.c'];
const waits = bufText.split('sceGxmNotificationWait(&query_fence);').length - 1;
const guardedWaits = (bufText.match(/if \(scene_reset\(\) \|\| q->sync <= query_fence\.value\)\n\t+sceGxmNotificationWait\(&query_fence\);/g) || []).length;
if (waits !== guardedWaits || waits < 2) fail('query fence waits are not all guarded (' + guardedWaits + ' of ' + waits + ')');

// ---- audit 6 (patch 0012, when the tree has it): low precision for fragment shaders only is applied where the precision header
// is sized and where it is written, vglUseLowPrecision keeps its old meaning, and the new call sets both flags
{
    const glsl = read('utils/glsl_utils.c');
    if (/glsl_precision_fs_only/.test(glsl)) {
        const gated = (glsl.match(/if \(glsl_precision_low && \(!glsl_precision_fs_only \|\| s->type == GL_FRAGMENT_SHADER\)\)\n\t+(size \+= strlen|strcat\(s->source,) glsl_?precision_hdr|if \(glsl_precision_low && \(!glsl_precision_fs_only \|\| s->type == GL_FRAGMENT_SHADER\)\)\n\t+(size \+= strlen\(glsl_precision_hdr\)|strcat\(s->source, glsl_precision_hdr\))/g) || []).length;
        const bare = (glsl.match(/if \(glsl_precision_low\)\n/g) || []).length;
        if (gated !== 2 || bare !== 0) fail('the precision header must be gated on the shader type in both places (found ' + gated + ' gated, ' + bare + ' not)');
        const custom = files['custom_shaders.c'];
        if (!/void vglUseLowPrecision\(GLboolean val\) \{\n\tglsl_precision_low = val;\n\tglsl_precision_fs_only = GL_FALSE;\n\}/.test(custom)) fail('vglUseLowPrecision must clear the fragment-only flag');
        if (!/void vglUseLowPrecisionFragmentOnly\(GLboolean val\) \{\n\tglsl_precision_low = val;\n\tglsl_precision_fs_only = val;\n\}/.test(custom)) fail('vglUseLowPrecisionFragmentOnly must set both flags');
    }
}

// ---- the fixture
const hasMs = /init_depth_stencil_buffer_ms\(/.test(files['gxm.c']);
const hasHint = /apply_depth_hint\(/.test(files['gxm.c']);
const parts = [];
parts.push(functionText('gxm.c', '(?:static inline __attribute__\\(\\(always_inline\\)\\) )?void scene_end\\(void\\) \\{', 'scene_end'));
if (hasMs) parts.push(functionText('gxm.c', 'GLboolean init_depth_stencil_buffer_ms\\([^)]*\\) \\{', 'init_depth_stencil_buffer_ms'));
parts.push(functionText('gxm.c', 'GLboolean init_depth_stencil_buffer\\([^)]*\\) \\{', 'init_depth_stencil_buffer'));
parts.push(functionText('gxm.c', 'void init_display_depth_stencil_surfaces\\(void\\) \\{', 'init_display_depth_stencil_surfaces'));
const gx = files['gxm.c'];
const blockStart = gx.indexOf('// A scene that could not be begun');
const blockEnd = gx.indexOf('GLboolean scene_reset(void) {');
if (blockStart < 0 || blockEnd < blockStart) throw new Error('scene failure block missing: apply patch 0009');
parts.push(gx.slice(blockStart, blockEnd));
parts.push(resetText);
parts.push(functionText('gxm.c', 'void vglSubmitWithoutSwap\\(void\\) \\{', 'vglSubmitWithoutSwap'));
parts.push(functionText('gxm.c', 'void glFinish\\(void\\) \\{', 'glFinish'));
parts.push(functionText('gxm.c', 'void glFlush\\(void\\) \\{', 'glFlush'));
parts.push(functionText('draw.c', 'void glDrawArrays\\(GLenum mode, GLint first, GLsizei count\\) \\{', 'glDrawArrays'));
parts.push(functionText('draw.c', 'void glMultiDrawArrays\\([^)]*\\) \\{', 'glMultiDrawArrays'));
parts.push(functionText('draw.c', 'void glDrawArraysInstanced\\([^)]*\\) \\{', 'glDrawArraysInstanced'));
parts.push(functionText('misc.c', 'void glClear\\(GLbitfield mask\\) \\{', 'glClear'));
parts.push(functionText('framebuffers.c', 'void glBlitNamedFramebuffer\\([^)]*\\) \\{', 'glBlitNamedFramebuffer'));
// ---- the GXM state cache (patch 0011), when the tree has it: the real wrapper block of shared.h and the real definitions of gxm.c go
// in front of every driver function (so that their setter calls are the wrapped ones), and the real state-setting part of the
// custom shader draw goes into a function of its own (tests/vitagl_scene_fail_test.cpp drives it with its world of programs and
// textures).
const marks = (text, name) => {
    const b = text.indexOf('/* ' + name + ' begin */'), e = text.indexOf('/* ' + name + ' end */');
    if (b < 0 || e < b) return null;
    return text.slice(b, e + ('/* ' + name + ' end */').length);
};
const scBlock = marks(read('shared.h'), 'GXM_STATE_CACHE'), scDefs = marks(files['gxm.c'], 'GXM_STATE_CACHE');
const hasSc = !!(scBlock && scDefs);
let drawSlice = '';
if (hasSc) {
    // every setter in the tree goes through the wrappers: no file calls the sceGxm function of a cached setter behind them
    const cached = ['sceGxmSetFragmentProgram', 'sceGxmSetVertexProgram', 'sceGxmSetFragmentTexture'];
    for (const [name, text] of Object.entries(files)) {
        if (name === 'splashscreen.c') continue; // (its own context, which the wrappers pass through)
        for (const fn of cached) {
            const re = new RegExp('[^\\n]*\\b' + fn + '\\((?!gxm_context\\b)[^\\n]*', 'g');
            let m;
            while ((m = re.exec(text))) fail('a ' + fn + ' call on another context than gxm_context in ' + name + ': ' + m[0].trim());
        }
    }
    const fn = functionText('custom_shaders.c', 'GLboolean _glDrawElements_CustomShadersIMPL\\([^)]*\\) \\{', '_glDrawElements_CustomShadersIMPL');
    const startMark = '\tsetup_frag_program();\n';
    const start = fn.indexOf(startMark);
    const end = fn.indexOf('#if defined(HAVE_VERTEX_LAYOUT_CACHE)', start);
    if (start < 0 || end < start) fail('the custom shader draw lacks setup_frag_program() before its vertex layout cache');
    const vpLine = fn.match(/\n\tsceGxmSetVertexProgram\(gxm_context, p->vprog\);\n/);
    if (!vpLine) fail('the custom shader draw does not set the vertex program the expected way');
    const msaaDefs = files['custom_shaders.c'].indexOf('#ifdef FBO_MSAA\n#define FRAG_MSAA_DIRTY'); // (the macro reads them when the tree has FBO_MSAA)
    const setupAt = files['custom_shaders.c'].indexOf('#define setup_frag_program()');
    const macroStart = msaaDefs >= 0 && msaaDefs < setupAt ? msaaDefs : setupAt;
    const macroEnd = files['custom_shaders.c'].indexOf('#define align_attributes', macroStart);
    if (macroStart < 0 || macroEnd < macroStart) fail('setup_frag_program macro not found');
    drawSlice = files['custom_shaders.c'].slice(macroStart, macroEnd) + '\n' +
        'static void fixture_draw_state(program *p) {\n' + fn.slice(start, end) + '\n\tp->vprog = fixture_pick_vprog(p);\n\tsceGxmSetVertexProgram(gxm_context, p->vprog);\n}\n';
    parts.unshift(scBlock);
    parts.unshift(scDefs);
}
fs.writeFileSync(output, parts.join('\n\n') + '\n');
if (hasSc) fs.writeFileSync(output.replace(/\.inc$/, '_draw.inc'), drawSlice);
fs.writeFileSync(output.replace(/\.inc$/, '_config.h'),
    '#define FIXTURE_HAS_MS ' + (hasMs ? 1 : 0) + '\n#define FIXTURE_HAS_DEPTH_HINT ' + (hasHint ? 1 : 0) + '\n' + '#define FIXTURE_HAS_SC ' + (hasSc ? 1 : 0) + '\n' + (hasMs ? '#define FBO_MSAA 1\n' : ''));
console.log('scene fixture prepared: ' + sceneResetCalls + ' scene_reset callers audited, ms=' + hasMs + ' hint=' + hasHint);
