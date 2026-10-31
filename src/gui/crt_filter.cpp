// CPCSyntaxError — the advanced CRT filter (crt_filter.h).
//
// Stage 1, the signal: each pixel turned into luma and two colour-difference signals, the
// luma spread a little to the pixels either side and the colour averaged over four pixels and
// with the line above (as a PAL decoder's delay line does), the odd lines' colour turned and
// scaled by the PAL phase error; then the lines drawn with a darker line between each two
// (when the picture is drawn at least twice its height), through brightness, contrast and gamma.
// Stage 2, the screen: each pixel's light bled into its neighbours and the bright ones glowing
// further, then the glass -- bulged, its corners rounded (a lit dot left in each dark corner),
// dark lines every few screen lines, the phosphor mask, a tint and a rolling bright bar.
#include "crt_filter.h"

#include <SDL.h>
#include <SDL_opengl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <type_traits>

namespace cpcse {

// ==================================================================== settings
const CrtFilterSlider CRT_FILTER_SLIDERS[] = {
    { "blur",           "Blur",                     &CrtFilterSettings::blur,           0, 1000, true },
    { "scanlineshade",  "Scanline shade",           &CrtFilterSettings::scanlineShade,  0, 1000, true },
    { "saturation",     "Saturation",               &CrtFilterSettings::saturation,     0, 2000, true },
    { "contrast",       "Contrast",                 &CrtFilterSettings::contrast,       0, 2000, true },
    { "brightness",     "Brightness",               &CrtFilterSettings::brightness,     0, 2000, true },
    { "gamma",          "Gamma",                    &CrtFilterSettings::gamma,          0, 4000, true },
    { "hue",            "Tint",                     &CrtFilterSettings::hue,            0, 2000, true },
    { "oddlinesphase",  "PAL odd line phase",       &CrtFilterSettings::oddLinesPhase,  0, 2000, true },
    { "oddlinesoffset", "PAL odd line offset",      &CrtFilterSettings::oddLinesOffset, 0, 2000, true },
    { "scale",          "Pattern scale",            &CrtFilterSettings::scale,          1, 4,    false },
    { "everynlines",    "Scan lines: every n lines",&CrtFilterSettings::everyNLines,    2, 8,    false },
    { "linesdarkened",  "Lines darkened",           &CrtFilterSettings::linesDarkened,  1, 4,    false },
    { "darkness",       "Darkness (%)",             &CrtFilterSettings::darkness,       0, 100,  false },
    { "bulge",          "Bulge (%)",                &CrtFilterSettings::bulge,          0, 100,  false },
    { "corners",        "Rounded corners (%)",      &CrtFilterSettings::corners,        0, 100,  false },
    { "cornerdot",      "Corner dot",               &CrtFilterSettings::cornerDot,      0, 4,    false },
    { "bleed",          "Bleed (%)",                &CrtFilterSettings::bleed,          0, 100,  false },
    { "glow",           "Glow (%)",                 &CrtFilterSettings::glow,           0, 100,  false },
    { "mask",           "Mask (%)",                 &CrtFilterSettings::mask,           0, 100,  false },
    { "maskwidth",      "Stripe width",             &CrtFilterSettings::maskWidth,      1, 4,    false },
    { "tint",           "Tint (%)",                 &CrtFilterSettings::tint,           0, 100,  false },
    { "roll",           "Roll per frame",           &CrtFilterSettings::roll,          -8, 8,    false },
    { "rollingbar",     "Rolling bar (%)",          &CrtFilterSettings::rollingBar,     0, 100,  false },
};
const int CRT_FILTER_SLIDER_COUNT = (int)(sizeof(CRT_FILTER_SLIDERS) / sizeof(CRT_FILTER_SLIDERS[0]));
const char* const CRT_FILTER_MASK_SHAPES[4] = { "Stripes", "Stripes with gaps", "Slots", "Dots" };

void CrtFilterSettings::load(const std::map<std::string, std::string>& ini, const std::string& prefix) {
    auto get = [&](const std::string& k, int& v, int lo, int hi) {
        auto it = ini.find(prefix + "crtfilter." + k);
        if (it != ini.end() && !it->second.empty()) v = std::clamp(std::atoi(it->second.c_str()), lo, hi);
    };
    int on = enabled, s1 = signal, s2 = screen, cols = columns;
    if (auto it = ini.find(prefix + "crtfilter"); it != ini.end()) on = std::atoi(it->second.c_str()) != 0;
    get("signal", s1, 0, 1);
    get("screen", s2, 0, 1);
    get("columns", cols, 0, 1);
    enabled = on; signal = s1 != 0; screen = s2 != 0; columns = cols != 0;
    for (int i = 0; i < CRT_FILTER_SLIDER_COUNT; i++) {
        const CrtFilterSlider& s = CRT_FILTER_SLIDERS[i];
        get(s.key, this->*s.field, s.min, s.max);
    }
    get("maskshape", maskShape, 0, 3);
    if (auto it = ini.find(prefix + "crtfilter.tintcolour"); it != ini.end() && !it->second.empty())
        tintColour = (uint32_t)std::strtoul(it->second.c_str(), nullptr, 16) & 0xffffff;
}

void CrtFilterSettings::save(std::ostream& out, const std::string& prefix) const {
    const std::string p = prefix + "crtfilter";
    out << p << "=" << (enabled ? 1 : 0) << "\n"
        << p << ".signal=" << (signal ? 1 : 0) << "\n"
        << p << ".screen=" << (screen ? 1 : 0) << "\n"
        << p << ".columns=" << (columns ? 1 : 0) << "\n"
        << p << ".maskshape=" << maskShape << "\n";
    char hex[8];
    std::snprintf(hex, sizeof(hex), "%06x", (unsigned)(tintColour & 0xffffff));
    out << p << ".tintcolour=" << hex << "\n";
    for (int i = 0; i < CRT_FILTER_SLIDER_COUNT; i++)
        out << p << "." << CRT_FILTER_SLIDERS[i].key << "=" << this->*CRT_FILTER_SLIDERS[i].field << "\n";
}

bool ScreenLook::load(const std::map<std::string, std::string>& ini, const std::string& prefix) {
    bool any = false;
    auto find = [&](const char* k) -> const std::string* {
        auto it = ini.find(prefix + k);
        if (it == ini.end() || it->second.empty()) return nullptr;
        any = true;
        return &it->second;
    };
    auto flag = [&](const char* k, bool& v) { if (const std::string* s = find(k)) v = std::atoi(s->c_str()) != 0; };
    auto level = [&](const char* k, float& v) { if (const std::string* s = find(k)) v = std::clamp((float)std::atof(s->c_str()), 0.0f, 1.0f); };
    flag("integerscale", integerScale);
    flag("aspect", maintainAspect);
    flag("crt", crtEffect);
    level("crtscanline", crtScanline);
    level("crtbloom", crtBloom);
    level("crtblur", crtBlur);
    if (const std::string* s = find("crtpattern"))
        crtPattern = (*s == "slot" || *s == "soft" || *s == "scanlines" || *s == "none") ? *s : "auto";
    level("crtmask", crtMask);
    if (const std::string* s = find("crtmasksize")) crtMaskSize = std::clamp(std::atoi(s->c_str()), 1, 4);
    flag("crtmaskturned", crtMaskTurned);
    if (find("crtfilter")) crtFilter.load(ini, prefix);
    return any;
}

void ScreenLook::save(std::ostream& out, const std::string& prefix) const {
    out << prefix << "integerscale=" << (integerScale ? 1 : 0) << "\n"
        << prefix << "aspect=" << (maintainAspect ? 1 : 0) << "\n"
        << prefix << "crt=" << (crtEffect ? 1 : 0) << "\n"
        << prefix << "crtscanline=" << crtScanline << "\n"
        << prefix << "crtbloom=" << crtBloom << "\n"
        << prefix << "crtblur=" << crtBlur << "\n"
        << prefix << "crtpattern=" << crtPattern << "\n"
        << prefix << "crtmask=" << crtMask << "\n"
        << prefix << "crtmasksize=" << crtMaskSize << "\n"
        << prefix << "crtmaskturned=" << (crtMaskTurned ? 1 : 0) << "\n";
    crtFilter.save(out, prefix);
}

// ==================================================================== shaders
namespace {

// A quad from gl_VertexID alone: no vertex buffer (a core profile still wants a VAO bound).
const char* VS = R"(
out vec2 v_uv;
void main() {
  vec2 p = vec2((gl_VertexID & 1) != 0 ? 1.0 : -1.0, (gl_VertexID & 2) != 0 ? 1.0 : -1.0);
  v_uv = p * 0.5 + 0.5;
  gl_Position = vec4(p, 0.0, 1.0);
})";

// Stage 1: the PAL decode, one texel per Mode 2 pixel and per line. The emulator's picture
// has each line twice: u_rowStep rows a line.
const char* FS_DECODE = R"(
in vec2 v_uv;
out vec4 o_colour;
uniform sampler2D u_fb;
uniform vec2 u_fbSize;
uniform vec2 u_size;
uniform float u_rowStep;
uniform float u_lf;
uniform float u_sat;
uniform float u_tint;
uniform vec2 u_rot;
uniform float u_offs;

vec3 px(float x, float y, float odd) {
  vec3 c = texture(u_fb, (vec2(x, y * u_rowStep) + 0.5) / u_fbSize).rgb;
  float l = dot(c, vec3(0.299, 0.587, 0.114));
  vec2 ch = vec2((c.b - l) * 0.564, (c.r - l) * 0.713) * u_sat;
  if (abs(ch.x) + abs(ch.y) > 0.001) ch.y += u_tint;
  if (odd > 0.5) ch = vec2(ch.x * u_rot.x - ch.y * u_rot.y, ch.x * u_rot.y + ch.y * u_rot.x) * u_offs;
  return vec3(l, ch);
}
vec2 chroma(float x, float y, float odd) {
  return (px(x - 2.0, y, odd).yz + px(x - 1.0, y, odd).yz +
          px(x, y, odd).yz + px(x + 1.0, y, odd).yz) * 0.25;
}
void main() {
  vec2 p = floor(v_uv * u_size);
  float odd = mod(p.y, 2.0);
  float y = px(p.x - 1.0, p.y, odd).x * u_lf +
            px(p.x, p.y, odd).x * (1.0 - 2.0 * u_lf) +
            px(p.x + 1.0, p.y, odd).x * u_lf;
  vec2 c = (chroma(p.x, p.y, odd) + chroma(p.x, p.y - 1.0, 1.0 - odd)) * 0.5;
  vec3 rgb = vec3(y + 1.402 * c.y, y - 0.344136 * c.x - 0.714136 * c.y, y + 1.772 * c.x);
  o_colour = vec4(clamp(rgb, 0.0, 1.0), 1.0);
})";

// No stage 1: the lines as they are, one texel each.
const char* FS_PLAIN = R"(
in vec2 v_uv;
out vec4 o_colour;
uniform sampler2D u_fb;
uniform vec2 u_fbSize;
uniform vec2 u_size;
uniform float u_rowStep;
void main() {
  vec2 p = floor(v_uv * u_size);
  o_colour = vec4(texture(u_fb, (vec2(p.x, p.y * u_rowStep) + 0.5) / u_fbSize).rgb, 1.0);
})";

const char* FS_SCANLINES = R"(
in vec2 v_uv;
out vec4 o_colour;
uniform sampler2D u_src;
uniform vec2 u_srcSize;
uniform vec2 u_outSize;
uniform float u_double;
uniform float u_rows;
uniform float u_shade;
uniform float u_bri;
uniform float u_con;
uniform float u_gam;

vec3 at(float x, float y) {
  return texture(u_src, (vec2(x, y) + 0.5) / u_srcSize).rgb;
}
vec3 curve(vec3 v) {
  return pow(max((v + u_bri) * u_con, 0.0), vec3(u_gam));
}
void main() {
  vec2 o = floor(v_uv * u_outSize);
  if (u_double < 0.5) { o_colour = vec4(curve(at(o.x, o.y)), 1.0); return; }
  float sx = floor(o.x * 0.5), sy = floor(o.y / u_rows);
  bool oddX = mod(o.x, 2.0) > 0.5;
  vec3 a = at(sx, sy);
  if (oddX) a = (a + at(sx + 1.0, sy)) * 0.5;
  if (o.y - sy * u_rows >= ceil(u_rows * 0.5)) {
    vec3 b = at(sx, sy + 1.0);
    if (oddX) b = (b + at(sx + 1.0, sy + 1.0)) * 0.5;
    o_colour = vec4(curve((a + b) * 0.5) * u_shade, 1.0);
  } else {
    o_colour = vec4(curve(a), 1.0);
  }
})";

// No stage 2: the picture scaled to the screen, each texel's edge kept sharp.
const char* FS_PRESENT = R"(
in vec2 v_uv;
out vec4 o_colour;
uniform sampler2D u_src;
uniform vec2 u_srcSize;
uniform vec2 u_scale;
void main() {
  vec2 texel = v_uv * u_srcSize;
  vec2 base = floor(texel);
  vec2 d = fract(texel) - 0.5;
  vec2 range = max(0.5 - 0.5 / u_scale, 0.0);
  vec2 f = (d - clamp(d, -range, range)) * max(u_scale, 1.0) + 0.5;
  o_colour = vec4(texture(u_src, (base + f) / u_srcSize).rgb, 1.0);
})";

// Stage 2a: bleed into the neighbours, and glow from the brighter ones further off.
const char* FS_BEAM = R"(
out vec4 o_colour;
uniform sampler2D u_frame;
uniform vec4 u_beam[2];
vec3 at(vec2 t, vec2 o) {
  return texture(u_frame, t + o * u_beam[0].xy).rgb;
}
void main() {
  vec2 t = (floor(gl_FragCoord.xy) + 0.5) * u_beam[0].xy;
  vec2 sx = vec2(u_beam[0].z, 0.0);
  vec2 sy = vec2(0.0, u_beam[0].w);
  vec2 a = u_beam[1].w > 0.5 ? sx : sy;
  vec2 b = u_beam[1].w > 0.5 ? sy : sx;
  vec3 c = at(t, vec2(0.0, 0.0));
  vec3 col = (c + (at(t, a) + at(t, -a)) * u_beam[1].x
                + (at(t, b) + at(t, -b)) * u_beam[1].y)
             / (1.0 + 2.0 * (u_beam[1].x + u_beam[1].y));
  vec3 light = vec3(0.0);
  for (int y = -2; y <= 2; y++) {
    for (int x = -2; x <= 2; x++) {
      if (x != 0 || y != 0) {
        light += max(at(t, float(x) * sx + float(y) * sy) - c, 0.0) / (1.0 + float(x * x + y * y));
      }
    }
  }
  o_colour = vec4(col + light * u_beam[1].z, 1.0);
})";

// Stage 2b: the glass. Drawn into a texture whose row 0 is the top, so p is the fragment.
const char* FS_SCREEN = R"(
out vec4 o_colour;
uniform sampler2D u_src;
uniform vec2 u_srcSize;
uniform vec2 u_scale;
uniform vec4 u_screen[8];

float wrap(float v, float period) {
  return v - period * floor((v + 0.5) / period);
}

vec3 picture(vec2 uv) {
  vec2 texel = uv * u_srcSize;
  vec2 base = floor(texel);
  vec2 d = fract(texel) - 0.5;
  vec2 range = max(0.5 - 0.5 / u_scale, 0.0);
  vec2 f = (d - clamp(d, -range, range)) * max(u_scale, 1.0) + 0.5;
  return texture(u_src, (base + f) / u_srcSize).rgb;
}

void main() {
  vec4 rect = u_screen[0];
  vec4 glass = u_screen[1];
  vec4 scan = u_screen[2];
  vec4 phosphor = u_screen[3];
  vec4 tint = u_screen[4];
  vec4 bar = u_screen[5];
  vec4 pattern = u_screen[6];
  vec4 cornerDot = u_screen[7];
  vec2 p = floor(gl_FragCoord.xy);
  vec2 halfSize = rect.zw * 0.5;
  vec2 d = p + 0.5 - halfSize;
  vec2 uv = d / halfSize;
  vec2 s = uv * ((1.0 + glass.x * dot(uv, uv)) * glass.y);
  vec3 c = picture(s * 0.5 + 0.5);
  vec2 corner = abs(d) - (halfSize - glass.z);
  float lit = 1.0;
  if (max(abs(s.x), abs(s.y)) > 1.0 || (corner.x > 0.0 && corner.y > 0.0 && dot(corner, corner) > glass.w)) {
    lit = 0.0;
  }
  vec2 dotOffset = halfSize - abs(d) - cornerDot.xy;
  if (dot(dotOffset, dotOffset) < pattern.w) lit = 1.0;
  float along = phosphor.y > 0.5 ? p.y : p.x;
  float across = phosphor.y > 0.5 ? p.x : p.y;
  if (wrap(along - scan.w, scan.x) < scan.y) c *= scan.z;
  float u = floor((across + 0.5) / pattern.x);
  float v = floor((along + 0.5) / pattern.x);
  float w = phosphor.z;
  float column = floor((u + 0.5) / w);
  float ph = wrap(column, 3.0);
  if (phosphor.w > 2.5) {
    float band = wrap(floor((v + 0.5) / w), 2.0);
    ph = wrap(floor((u + band * pattern.y + 0.5) / w), 3.0);
  } else if (phosphor.w > 1.5) {
    float triad = floor((u + 0.5) / (3.0 * w));
    if (wrap(v + triad * 2.0 * w, 4.0 * w) > 3.0 * w - 0.5) ph = 3.0;
  } else if (phosphor.w > 0.5) {
    ph = wrap(column, 4.0);
  }
  vec3 own = vec3(ph < 0.5 ? 1.0 : 0.0, ph > 0.5 && ph < 1.5 ? 1.0 : 0.0, ph > 1.5 && ph < 2.5 ? 1.0 : 0.0);
  c *= (1.0 - phosphor.x) + phosphor.x * own;
  c *= tint.rgb;
  float db = abs(along + 0.5 - bar.y);
  db = min(db, bar.w - db);
  c += (1.0 - c) * (bar.x * clamp(1.0 - db / bar.z, 0.0, 1.0));
  o_colour = vec4(c * lit, 1.0);
})";

// Whether the glass at (x, y) shows the picture: inside the bulged edge and the rounded corner.
bool isLit(double x, double y, double hx, double hy, double k, double radius) {
    const double dx = x - hx, dy = y - hy;
    const double u = dx / hx, v = dy / hy;
    const double f = (1 + k * (u * u + v * v)) / (1 + k);
    const double cx = std::abs(dx) - (hx - radius), cy = std::abs(dy) - (hy - radius);
    return std::abs(u * f) <= 1 && std::abs(v * f) <= 1 && !(cx > 0 && cy > 0 && cx * cx + cy * cy > radius * radius);
}

// How far along the diagonal from a corner the glass stays dark.
double darkCornerReach(double width, double height, double k, double radius) {
    const double hx = width * 0.5, hy = height * 0.5, len = std::hypot(hx, hy);
    for (double t = 0; t < len; t += 0.25)
        if (isLit(t * hx / len, t * hy / len, hx, hy, k, radius)) return t;
    return len;
}

// The GL 2+ entry points the filter uses, from the context (Windows' opengl32 exports 1.1).
struct Gl {
    PFNGLCREATESHADERPROC CreateShader = nullptr;
    PFNGLSHADERSOURCEPROC ShaderSource = nullptr;
    PFNGLCOMPILESHADERPROC CompileShader = nullptr;
    PFNGLGETSHADERIVPROC GetShaderiv = nullptr;
    PFNGLGETSHADERINFOLOGPROC GetShaderInfoLog = nullptr;
    PFNGLDELETESHADERPROC DeleteShader = nullptr;
    PFNGLCREATEPROGRAMPROC CreateProgram = nullptr;
    PFNGLATTACHSHADERPROC AttachShader = nullptr;
    PFNGLLINKPROGRAMPROC LinkProgram = nullptr;
    PFNGLGETPROGRAMIVPROC GetProgramiv = nullptr;
    PFNGLGETPROGRAMINFOLOGPROC GetProgramInfoLog = nullptr;
    PFNGLDELETEPROGRAMPROC DeleteProgram = nullptr;
    PFNGLUSEPROGRAMPROC UseProgram = nullptr;
    PFNGLGETUNIFORMLOCATIONPROC GetUniformLocation = nullptr;
    PFNGLUNIFORM1IPROC Uniform1i = nullptr;
    PFNGLUNIFORM1FPROC Uniform1f = nullptr;
    PFNGLUNIFORM2FPROC Uniform2f = nullptr;
    PFNGLUNIFORM4FVPROC Uniform4fv = nullptr;
    PFNGLGENFRAMEBUFFERSPROC GenFramebuffers = nullptr;
    PFNGLDELETEFRAMEBUFFERSPROC DeleteFramebuffers = nullptr;
    PFNGLBINDFRAMEBUFFERPROC BindFramebuffer = nullptr;
    PFNGLFRAMEBUFFERTEXTURE2DPROC FramebufferTexture2D = nullptr;
    PFNGLCHECKFRAMEBUFFERSTATUSPROC CheckFramebufferStatus = nullptr;
    PFNGLGENVERTEXARRAYSPROC GenVertexArrays = nullptr;
    PFNGLDELETEVERTEXARRAYSPROC DeleteVertexArrays = nullptr;
    PFNGLBINDVERTEXARRAYPROC BindVertexArray = nullptr;
    PFNGLACTIVETEXTUREPROC ActiveTexture = nullptr;

    bool load() {
        bool ok = true;
        auto get = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(SDL_GL_GetProcAddress(name));
            ok = ok && fn != nullptr;
        };
        get(CreateShader, "glCreateShader"); get(ShaderSource, "glShaderSource"); get(CompileShader, "glCompileShader");
        get(GetShaderiv, "glGetShaderiv"); get(GetShaderInfoLog, "glGetShaderInfoLog"); get(DeleteShader, "glDeleteShader");
        get(CreateProgram, "glCreateProgram"); get(AttachShader, "glAttachShader"); get(LinkProgram, "glLinkProgram");
        get(GetProgramiv, "glGetProgramiv"); get(GetProgramInfoLog, "glGetProgramInfoLog"); get(DeleteProgram, "glDeleteProgram");
        get(UseProgram, "glUseProgram"); get(GetUniformLocation, "glGetUniformLocation");
        get(Uniform1i, "glUniform1i"); get(Uniform1f, "glUniform1f"); get(Uniform2f, "glUniform2f"); get(Uniform4fv, "glUniform4fv");
        get(GenFramebuffers, "glGenFramebuffers"); get(DeleteFramebuffers, "glDeleteFramebuffers");
        get(BindFramebuffer, "glBindFramebuffer"); get(FramebufferTexture2D, "glFramebufferTexture2D");
        get(CheckFramebufferStatus, "glCheckFramebufferStatus");
        get(GenVertexArrays, "glGenVertexArrays"); get(DeleteVertexArrays, "glDeleteVertexArrays");
        get(BindVertexArray, "glBindVertexArray"); get(ActiveTexture, "glActiveTexture");
        return ok;
    }
};

struct Program {
    GLuint id = 0;
    std::map<std::string, GLint> uniforms;
};

struct Target {
    GLuint tex = 0, fbo = 0;
    int w = 0, h = 0;
};

} // namespace

struct CrtFilterRenderer::Impl {
    Gl gl;
    bool ready = false;
    GLuint vao = 0;
    Program decode, plain, scan, present, beam, screen;
    Target rtA, rtA2, rtB, rtBeam, rtOut;
    // the rolling lines and bar, and the dark corner's reach for the last size
    int lineRoll = 0, barRoll = 0;
    std::string reachKey;
    double reach = 0;

    GLuint compile(GLenum type, const std::string& src, std::string& error) {
        GLuint s = gl.CreateShader(type);
        const char* text = src.c_str();
        gl.ShaderSource(s, 1, &text, nullptr);
        gl.CompileShader(s);
        GLint ok = 0;
        gl.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[1024] = {};
            gl.GetShaderInfoLog(s, sizeof(log), nullptr, log);
            error = log;
            gl.DeleteShader(s);
            return 0;
        }
        return s;
    }

    bool build(Program& p, const char* version, const char* fs, std::string& error) {
        const std::string head = std::string(version) + "\n";
        GLuint v = compile(GL_VERTEX_SHADER, head + VS, error);
        GLuint f = v ? compile(GL_FRAGMENT_SHADER, head + fs, error) : 0;
        if (!v || !f) { if (v) gl.DeleteShader(v); return false; }
        p.id = gl.CreateProgram();
        gl.AttachShader(p.id, v);
        gl.AttachShader(p.id, f);
        gl.LinkProgram(p.id);
        gl.DeleteShader(v);
        gl.DeleteShader(f);
        GLint ok = 0;
        gl.GetProgramiv(p.id, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[1024] = {};
            gl.GetProgramInfoLog(p.id, sizeof(log), nullptr, log);
            error = log;
            return false;
        }
        return true;
    }

    GLint uniform(Program& p, const char* name) {
        auto it = p.uniforms.find(name);
        if (it != p.uniforms.end()) return it->second;
        return p.uniforms[name] = gl.GetUniformLocation(p.id, name);
    }

    void makeTarget(Target& t) {
        glGenTextures(1, &t.tex);
        glBindTexture(GL_TEXTURE_2D, t.tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl.GenFramebuffers(1, &t.fbo);
    }

    bool size(Target& t, int w, int h) {
        if (t.w == w && t.h == h) return true;
        glBindTexture(GL_TEXTURE_2D, t.tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        gl.BindFramebuffer(GL_FRAMEBUFFER, t.fbo);
        gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.tex, 0);
        if (gl.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { t.w = t.h = 0; return false; }
        t.w = w; t.h = h;
        return true;
    }

    static void filter(GLuint tex, GLint mode) {
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mode);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mode);
    }

    void into(const Target& t) {
        gl.BindFramebuffer(GL_FRAMEBUFFER, t.fbo);
        glViewport(0, 0, t.w, t.h);
    }

    void draw() { glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); }
};

CrtFilterRenderer::CrtFilterRenderer() : impl(new Impl) {}

CrtFilterRenderer::~CrtFilterRenderer() {
    // The context may be gone by now; the program's end frees the GL objects with it.
}

bool CrtFilterRenderer::init(const char* glslVersion) {
    Impl& m = *impl;
    if (m.ready) return true;
    if (!m.gl.load()) { error = "the graphics driver lacks the OpenGL functions the filter needs"; return false; }
    if (!m.build(m.decode, glslVersion, FS_DECODE, error) || !m.build(m.plain, glslVersion, FS_PLAIN, error)
        || !m.build(m.scan, glslVersion, FS_SCANLINES, error) || !m.build(m.present, glslVersion, FS_PRESENT, error)
        || !m.build(m.beam, glslVersion, FS_BEAM, error) || !m.build(m.screen, glslVersion, FS_SCREEN, error))
        return false;
    m.gl.GenVertexArrays(1, &m.vao);
    for (Target* t : { &m.rtA, &m.rtA2, &m.rtB, &m.rtBeam, &m.rtOut }) m.makeTarget(*t);
    m.ready = true;
    return true;
}

unsigned CrtFilterRenderer::render(unsigned source, int sourceW, int sourceH, int rowStep, int outW, int outH,
                                   float pixelScale, bool newFrame, const CrtFilterSettings& f) {
    Impl& m = *impl;
    Gl& gl = m.gl;
    if (!m.ready || outW < 1 || outH < 1 || sourceW < 1 || sourceH < rowStep || rowStep < 1) return 0;
    const int cropW = sourceW, cropH = sourceH / rowStep;

    // The GL state the interface's renderer finds: put back afterwards.
    GLint oldFbo = 0, oldProgram = 0, oldTex = 0, oldActive = 0, oldVao = 0, oldViewport[4] = {};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &oldFbo);
    glGetIntegerv(GL_CURRENT_PROGRAM, &oldProgram);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &oldActive);
    gl.ActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTex);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &oldVao);
    glGetIntegerv(GL_VIEWPORT, oldViewport);
    const GLenum caps[] = { GL_BLEND, GL_SCISSOR_TEST, GL_DEPTH_TEST, GL_CULL_FACE, GL_STENCIL_TEST };
    GLboolean capWas[5];
    for (int i = 0; i < 5; i++) { capWas[i] = glIsEnabled(caps[i]); glDisable(caps[i]); }
    gl.BindVertexArray(m.vao);

    bool ok = true;
    const Target* final = nullptr;

    // Stage 1 (or the lines as they are), one texel a pixel and a line.
    Target& rtA = f.signal ? m.rtA : m.rtA2;
    if ((ok = m.size(rtA, cropW, cropH))) {
        Program& p = f.signal ? m.decode : m.plain;
        m.into(rtA);
        gl.UseProgram(p.id);
        Impl::filter(source, GL_NEAREST);
        gl.Uniform1i(m.uniform(p, "u_fb"), 0);
        gl.Uniform2f(m.uniform(p, "u_fbSize"), (float)sourceW, (float)sourceH);
        gl.Uniform2f(m.uniform(p, "u_size"), (float)cropW, (float)cropH);
        gl.Uniform1f(m.uniform(p, "u_rowStep"), (float)rowStep);
        if (f.signal) {
            const double phase = (f.oddLinesPhase - 1000) * 0.045 * 3.14159265358979323846 / 180.0;
            gl.Uniform1f(m.uniform(p, "u_lf"), (float)(std::floor(64.0 * f.blur / 1000.0) / 255.0));
            gl.Uniform1f(m.uniform(p, "u_sat"), f.saturation / 1000.0f);
            gl.Uniform1f(m.uniform(p, "u_tint"), (float)(((f.hue * 50.0) / 2000.0 - 25.0) / 255.0));
            gl.Uniform2f(m.uniform(p, "u_rot"), (float)std::cos(phase), (float)std::sin(phase));
            gl.Uniform1f(m.uniform(p, "u_offs"), f.oddLinesOffset / 1000.0f);
        }
        m.draw();
        final = &rtA;
    }

    // Stage 1's scanlines, brightness, contrast and gamma.
    if (ok && f.signal) {
        const int rows = std::max(2, outH / cropH);
        const bool dbl = f.scanlineShade < 1000 && outH >= cropH * 2;
        const int ow = dbl ? cropW * 2 : cropW, oh = dbl ? cropH * rows : cropH;
        if ((ok = m.size(m.rtB, ow, oh))) {
            Program& p = m.scan;
            m.into(m.rtB);
            gl.UseProgram(p.id);
            Impl::filter(rtA.tex, GL_NEAREST);
            gl.Uniform1i(m.uniform(p, "u_src"), 0);
            gl.Uniform2f(m.uniform(p, "u_srcSize"), (float)cropW, (float)cropH);
            gl.Uniform2f(m.uniform(p, "u_outSize"), (float)ow, (float)oh);
            gl.Uniform1f(m.uniform(p, "u_double"), dbl ? 1.0f : 0.0f);
            gl.Uniform1f(m.uniform(p, "u_rows"), (float)rows);
            gl.Uniform1f(m.uniform(p, "u_shade"), f.scanlineShade / 1000.0f);
            gl.Uniform1f(m.uniform(p, "u_bri"), (float)(((f.brightness - 1000) * 128.0) / 1000.0 / 255.0));
            gl.Uniform1f(m.uniform(p, "u_con"), f.contrast / 1000.0f);
            gl.Uniform1f(m.uniform(p, "u_gam"), (float)(f.gamma / 1000.0 / 2.8));
            m.draw();
            final = &m.rtB;
        }
    }

    if (ok) ok = m.size(m.rtOut, outW, outH);
    if (ok && f.screen) {
        // Stage 2's settings, as numbers for the shaders (the same sums as the original).
        const bool rows = !f.columns;
        const double bleed = f.bleed / 100.0;
        const float beamU[8] = { 1.0f / final->w, 1.0f / final->h, (float)final->w / cropW, (float)final->h / cropH,
                                 (float)bleed, (float)(bleed * 0.35), (float)(f.glow / 100.0 * 0.5), rows ? 1.0f : 0.0f };
        const int extent = rows ? outH : outW;
        const int step = newFrame ? f.roll : 0;
        const int scale = f.scale * std::max(1, (int)std::lround(pixelScale));
        int period = f.everyNLines;
        int dark = std::min(f.linesDarkened, period - 1);
        period *= scale;
        dark *= scale;
        const int stripe = f.maskWidth;
        const double k = f.bulge / 100.0 * 0.5;
        const double radius = f.corners / 100.0 * std::min(outW, outH) * 0.5;
        const int dot = f.cornerDot * scale;
        const std::string key = std::to_string(outW) + "," + std::to_string(outH) + "," + std::to_string(k) + "," + std::to_string(radius);
        if (key != m.reachKey) { m.reachKey = key; m.reach = darkCornerReach(outW, outH, k, radius); }
        double dotX = 0, dotY = 0, dotRadius2 = 0;
        const int gap = scale;
        if (dot > 0 && m.reach >= dot + gap + 2) {
            const double len = std::hypot((double)outW, (double)outH);
            const double t = m.reach - gap - dot / 2.0;
            auto snap = [&](double v) { return (dot & 1) ? std::floor(v) + 0.5 : std::round(v); };
            dotX = snap(t * outW / len);
            dotY = snap(t * outH / len);
            dotRadius2 = dot * dot / 4.0 + 0.01;
        }
        m.lineRoll = ((m.lineRoll + step) % period + period) % period;
        m.barRoll = (m.barRoll + step) % extent;
        const double barCentre = std::fmod(extent * 0.3 + m.barRoll + extent, (double)extent);
        const double tint = f.tint / 100.0;
        auto ch = [&](int shift) { return (float)(1.0 + (((f.tintColour >> shift) & 0xff) / 255.0 - 1.0) * tint); };
        const float screenU[32] = {
            0, 0, (float)outW, (float)outH,
            (float)k, (float)(1.0 / (1.0 + k)), (float)radius, (float)(radius * radius),
            (float)period, dark - 0.5f, (float)(1.0 - f.darkness / 100.0), (float)m.lineRoll,
            (float)(f.mask / 100.0), rows ? 1.0f : 0.0f, (float)stripe, (float)f.maskShape,
            ch(16), ch(8), ch(0), 1,
            (float)(f.rollingBar / 100.0), (float)barCentre, (float)std::max(extent / 8.0, 1.0), (float)extent,
            (float)scale, (float)((3 * stripe + 1) / 2), 0, (float)dotRadius2,
            (float)dotX, (float)dotY, 0, 0,
        };

        const Target* picture = final;
        if (f.bleed > 0 || f.glow > 0) {
            if ((ok = m.size(m.rtBeam, final->w, final->h))) {
                Program& p = m.beam;
                m.into(m.rtBeam);
                gl.UseProgram(p.id);
                Impl::filter(final->tex, GL_NEAREST);
                gl.Uniform1i(m.uniform(p, "u_frame"), 0);
                gl.Uniform4fv(m.uniform(p, "u_beam"), 2, beamU);
                m.draw();
                picture = &m.rtBeam;
            }
        }
        if (ok) {
            Program& p = m.screen;
            m.into(m.rtOut);
            gl.UseProgram(p.id);
            Impl::filter(picture->tex, GL_LINEAR);
            gl.Uniform1i(m.uniform(p, "u_src"), 0);
            gl.Uniform2f(m.uniform(p, "u_srcSize"), (float)picture->w, (float)picture->h);
            gl.Uniform2f(m.uniform(p, "u_scale"), (float)outW / picture->w, (float)outH / picture->h);
            gl.Uniform4fv(m.uniform(p, "u_screen"), 8, screenU);
            m.draw();
        }
    } else if (ok) {
        Program& p = m.present;
        m.into(m.rtOut);
        gl.UseProgram(p.id);
        Impl::filter(final->tex, GL_LINEAR);
        gl.Uniform1i(m.uniform(p, "u_src"), 0);
        gl.Uniform2f(m.uniform(p, "u_srcSize"), (float)final->w, (float)final->h);
        gl.Uniform2f(m.uniform(p, "u_scale"), (float)outW / final->w, (float)outH / final->h);
        m.draw();
    }

    // The source as main() keeps it, and the GL state as it was.
    Impl::filter(source, GL_NEAREST);
    gl.BindVertexArray((GLuint)oldVao);
    gl.BindFramebuffer(GL_FRAMEBUFFER, (GLuint)oldFbo);
    gl.UseProgram((GLuint)oldProgram);
    glBindTexture(GL_TEXTURE_2D, (GLuint)oldTex);
    gl.ActiveTexture((GLenum)oldActive);
    glViewport(oldViewport[0], oldViewport[1], oldViewport[2], oldViewport[3]);
    for (int i = 0; i < 5; i++) if (capWas[i]) glEnable(caps[i]);
    return ok ? m.rtOut.tex : 0;
}

} // namespace cpcse
