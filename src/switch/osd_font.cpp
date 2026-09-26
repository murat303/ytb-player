#include "newpipe/osd_font.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#ifdef __SWITCH__
#include <switch.h>
#endif

#include <SDL2/SDL.h>
#include <SDL2/SDL_opengles2.h>

#include "newpipe/log.hpp"

// A private, static copy: borealis compiles stb_truetype inside fontstash with fontstash's own
// allocator, which expects a fontstash context as user data.
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include <borealis/extern/nanovg/stb_truetype.h>

// Thumbnails, likewise a private static copy (nanovg compiles its own for its images).
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include <borealis/extern/nanovg/stb_image.h>

namespace newpipe {
namespace {

// Strings kept as textures; the clock and the seek target change every second.
constexpr size_t kMaxCachedTexts = 48;
constexpr GLenum kVertexArrayBinding = 0x85B5;  // GL_VERTEX_ARRAY_BINDING(_OES)

const char* kVertexShader =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "    v_uv = a_uv;\n"
    "    gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "}\n";

// Text textures hold coverage in alpha and take u_color; pictures keep their own colors.
// u_alpha fades either.
const char* kFragmentShader =
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D u_texture;\n"
    "uniform vec3 u_color;\n"
    "uniform float u_picture;\n"
    "uniform float u_alpha;\n"
    "void main() {\n"
    "    vec4 texel = texture2D(u_texture, v_uv);\n"
    "    vec4 color = mix(vec4(u_color, texel.a), texel, u_picture);\n"
    "    gl_FragColor = vec4(color.rgb, color.a * u_alpha);\n"
    "}\n";

// Height of the alpha ramps fade() stretches over its rectangle.
constexpr int kRampSize = 64;

// A new texture from pixels (GL_ALPHA text or GL_RGBA pictures). The binding of the active
// texture unit and the unpack alignment go back to what they were, since mpv shares the context.
GLuint upload_texture(GLenum format, int width, int height, const unsigned char* pixels) {
    GLint saved_texture = 0;
    GLint saved_unpack_alignment = 4;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &saved_texture);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &saved_unpack_alignment);
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, format, GL_UNSIGNED_BYTE, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, saved_texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, saved_unpack_alignment);
    return texture;
}

// Code points of a UTF-8 string; broken bytes are skipped.
std::vector<uint32_t> decode_utf8(const std::string& text) {
    std::vector<uint32_t> out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        size_t length = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 0;
        if (length == 0 || i + length > text.size()) {
            i++;
            continue;
        }
        uint32_t code = length == 1 ? lead : lead & (0xFF >> (length + 1));
        bool valid = true;
        for (size_t k = 1; k < length; k++) {
            const unsigned char next = static_cast<unsigned char>(text[i + k]);
            if ((next & 0xC0) != 0x80) {
                valid = false;
                break;
            }
            code = (code << 6) | (next & 0x3F);
        }
        i += valid ? length : 1;
        if (valid) {
            out.push_back(code);
        }
    }
    return out;
}

unsigned int compile(GLenum type, const char* source) {
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char text[256] = {};
        glGetShaderInfoLog(shader, sizeof(text), nullptr, text);
        logf("osd font: shader error %s", text);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

}  // namespace

struct OsdFont::FontInfo {
    stbtt_fontinfo info;
};

OsdFont::OsdFont() = default;

OsdFont::~OsdFont() {
    delete font_;
}

bool OsdFont::load() {
    if (ready_) {
        return true;
    }
    const unsigned char* data = nullptr;
#ifdef __SWITCH__
    PlFontData shared{};
    if (R_SUCCEEDED(plGetSharedFontByType(&shared, PlSharedFontType_Standard))) {
        data = static_cast<const unsigned char*>(shared.address);
    }
#else
    for (const char* path : {"resources/font/switch_font.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"}) {
        if (FILE* file = std::fopen(path, "rb")) {
            std::fseek(file, 0, SEEK_END);
            const long size = std::ftell(file);
            std::fseek(file, 0, SEEK_SET);
            file_data_.resize(size > 0 ? static_cast<size_t>(size) : 0);
            const bool read = size > 0 && std::fread(file_data_.data(), 1, file_data_.size(), file) == file_data_.size();
            std::fclose(file);
            if (read) {
                data = file_data_.data();
                break;
            }
        }
    }
#endif
    if (!data) {
        log_line("osd font: no font data, using the bitmap font");
        return false;
    }
    font_ = new FontInfo();
    if (!stbtt_InitFont(&font_->info, data, stbtt_GetFontOffsetForIndex(data, 0))) {
        log_line("osd font: stbtt_InitFont failed, using the bitmap font");
        delete font_;
        font_ = nullptr;
        return false;
    }
    ready_ = true;
    return true;
}

int OsdFont::measure(const std::string& utf8, int pixel_height) {
    if (!ready_ || utf8.empty()) {
        return 0;
    }
    const float scale = stbtt_ScaleForPixelHeight(&font_->info, static_cast<float>(pixel_height));
    float width = 0.0f;
    int previous = 0;
    for (const uint32_t code : decode_utf8(utf8)) {
        const int glyph = stbtt_FindGlyphIndex(&font_->info, static_cast<int>(code));
        // A character the font lacks (an emoji in a title) is left out, not drawn as a box.
        if (glyph == 0 && code >= 0x20) {
            continue;
        }
        if (previous) {
            width += scale * stbtt_GetGlyphKernAdvance(&font_->info, previous, glyph);
        }
        int advance = 0;
        int bearing = 0;
        stbtt_GetGlyphHMetrics(&font_->info, glyph, &advance, &bearing);
        width += scale * advance;
        previous = glyph;
    }
    return static_cast<int>(std::ceil(width));
}

const OsdFont::Text* OsdFont::text_for(const std::string& utf8, int pixel_height) {
    const std::string key = std::to_string(pixel_height) + '\x1f' + utf8;
    auto found = cache_.find(key);
    if (found != cache_.end()) {
        found->second.last_used = frame_;
        return &found->second;
    }

    const float scale = stbtt_ScaleForPixelHeight(&font_->info, static_cast<float>(pixel_height));
    int ascent = 0;
    int descent = 0;
    int line_gap = 0;
    stbtt_GetFontVMetrics(&font_->info, &ascent, &descent, &line_gap);
    const int baseline = static_cast<int>(std::ceil(ascent * scale));

    // Glyph boxes relative to the line's top-left corner. Accents on capitals (İ, Ö, Ü, Ğ) rise
    // above the ascent of the Switch font, and glyphs starting above the texture used to be
    // skipped: "İlk" read "lk" on the console. The texture now grows by that overhang and
    // draw() lifts it by as much, so the baseline stays where the layout expects it.
    struct Placed {
        int glyph;
        int left;
        int top;
        int width;
        int height;
    };
    std::vector<Placed> placed;
    int top_edge = 0;
    int bottom_edge = baseline + static_cast<int>(std::ceil(-descent * scale)) + 1;
    int right_edge = measure(utf8, pixel_height) + 2;
    float pen = 1.0f;
    int previous = 0;
    for (const uint32_t code : decode_utf8(utf8)) {
        const int glyph = stbtt_FindGlyphIndex(&font_->info, static_cast<int>(code));
        if (glyph == 0 && code >= 0x20) {
            continue;  // as in measure()
        }
        if (previous) {
            pen += scale * stbtt_GetGlyphKernAdvance(&font_->info, previous, glyph);
        }
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetGlyphBitmapBox(&font_->info, glyph, scale, scale, &x0, &y0, &x1, &y1);
        if (x1 > x0 && y1 > y0) {
            const Placed box{glyph, std::max(0, static_cast<int>(pen) + x0), baseline + y0, x1 - x0, y1 - y0};
            top_edge = std::min(top_edge, box.top);
            bottom_edge = std::max(bottom_edge, box.top + box.height);
            right_edge = std::max(right_edge, box.left + box.width);
            placed.push_back(box);
        }
        int advance = 0;
        int bearing = 0;
        stbtt_GetGlyphHMetrics(&font_->info, glyph, &advance, &bearing);
        pen += scale * advance;
        previous = glyph;
    }
    if (placed.empty()) {
        return nullptr;
    }

    const int overhang = -top_edge;
    const int width = right_edge;
    const int height = bottom_edge + overhang;
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height, 0);
    for (const Placed& box : placed) {
        // Glyphs rasterize into their own box; overlapping edges (kerning) take the maximum.
        std::vector<unsigned char> bitmap(static_cast<size_t>(box.width) * box.height, 0);
        stbtt_MakeGlyphBitmap(&font_->info, bitmap.data(), box.width, box.height, box.width, scale, scale, box.glyph);
        for (int row = 0; row < box.height; row++) {
            unsigned char* dst = &pixels[static_cast<size_t>(box.top + overhang + row) * width + box.left];
            const unsigned char* src = &bitmap[static_cast<size_t>(row) * box.width];
            for (int col = 0; col < box.width; col++) {
                dst[col] = std::max(dst[col], src[col]);
            }
        }
    }

    Text text;
    text.width = width;
    text.height = height;
    text.overhang = overhang;
    text.last_used = frame_;
    text.texture = upload_texture(GL_ALPHA, width, height, pixels.data());

    evict();
    return &(cache_[key] = text);
}

void OsdFont::evict() {
    while (cache_.size() >= kMaxCachedTexts) {
        auto oldest = std::min_element(cache_.begin(), cache_.end(), [](const auto& a, const auto& b) {
            return a.second.last_used < b.second.last_used;
        });
        glDeleteTextures(1, &oldest->second.texture);
        cache_.erase(oldest);
    }
}

bool OsdFont::ensure_program() {
    if (program_) {
        return true;
    }
    const GLuint vertex = compile(GL_VERTEX_SHADER, kVertexShader);
    const GLuint fragment = compile(GL_FRAGMENT_SHADER, kFragmentShader);
    if (!vertex || !fragment) {
        return false;
    }
    const GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        log_line("osd font: program link failed");
        glDeleteProgram(program);
        return false;
    }
    program_ = program;
    attr_pos_ = glGetAttribLocation(program, "a_pos");
    attr_uv_ = glGetAttribLocation(program, "a_uv");
    uniform_color_ = glGetUniformLocation(program, "u_color");
    uniform_texture_ = glGetUniformLocation(program, "u_texture");
    uniform_picture_ = glGetUniformLocation(program, "u_picture");
    uniform_alpha_ = glGetUniformLocation(program, "u_alpha");

    // Masks for fill() and fade(): one opaque texel, and ramps from clear to opaque downwards
    // and upwards. The ramps rise fast (t^0.55): over a bright video a straight one left the
    // text next to the progress bar on half a shade.
    const unsigned char opaque = 255;
    unsigned char ramp_down[kRampSize];
    unsigned char ramp_up[kRampSize];
    for (int i = 0; i < kRampSize; i++) {
        const float t = static_cast<float>(i) / (kRampSize - 1);
        ramp_down[i] = static_cast<unsigned char>(std::lround(255.0f * std::pow(t, 0.55f)));
        ramp_up[kRampSize - 1 - i] = ramp_down[i];
    }
    solid_texture_ = upload_texture(GL_ALPHA, 1, 1, &opaque);
    ramp_down_texture_ = upload_texture(GL_ALPHA, 1, kRampSize, ramp_down);
    ramp_up_texture_ = upload_texture(GL_ALPHA, 1, kRampSize, ramp_up);

    glGenBuffers(1, &vertex_buffer_);
    // GLES 3 has vertex arrays in core, GLES 2 through OES_vertex_array_object.
    const char* extensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    const bool core = version && std::strstr(version, "OpenGL ES 3");
    const bool extension = extensions && std::strstr(extensions, "GL_OES_vertex_array_object");
    if (core || extension) {
        const char* suffix = core ? "" : "OES";
        auto proc = [suffix](const char* name) { return SDL_GL_GetProcAddress((std::string(name) + suffix).c_str()); };
        auto gen = reinterpret_cast<void (*)(GLsizei, GLuint*)>(proc("glGenVertexArrays"));
        bind_vertex_array_ = reinterpret_cast<void (*)(GLuint)>(proc("glBindVertexArray"));
        delete_vertex_arrays_ = reinterpret_cast<void (*)(GLsizei, const GLuint*)>(proc("glDeleteVertexArrays"));
        if (gen && bind_vertex_array_) {
            GLint saved_vertex_array = 0;
            GLint saved_array_buffer = 0;
            glGetIntegerv(kVertexArrayBinding, &saved_vertex_array);
            glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &saved_array_buffer);
            gen(1, &vertex_array_);
            bind_vertex_array_(vertex_array_);
            glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer_);
            set_attributes();
            bind_vertex_array_(saved_vertex_array);
            glBindBuffer(GL_ARRAY_BUFFER, saved_array_buffer);
        }
    }
    logf("osd font: GL %s, own vertex array %s", version ? version : "?", vertex_array_ ? "yes" : "no");
    return true;
}

void OsdFont::draw(const std::string& utf8, int x, int y, int pixel_height, int screen_width, int screen_height,
                   float r, float g, float b) {
    if (!ready_ || utf8.empty() || !ensure_program()) {
        return;
    }
    frame_++;
    if (const Text* text = text_for(utf8, pixel_height)) {
        draw_quad(text->texture, false, x, y - text->overhang, text->width, text->height, screen_width,
                  screen_height, r, g, b, 1.0f);
    }
}

void OsdFont::fill(int x, int y, int width, int height, int screen_width, int screen_height, float r, float g,
                   float b, float alpha) {
    if (width > 0 && height > 0 && ensure_program()) {
        draw_quad(solid_texture_, false, x, y, width, height, screen_width, screen_height, r, g, b, alpha);
    }
}

void OsdFont::fade(int x, int y, int width, int height, int screen_width, int screen_height, float r, float g,
                   float b, float alpha, bool darker_at_top) {
    if (width > 0 && height > 0 && ensure_program()) {
        draw_quad(darker_at_top ? ramp_up_texture_ : ramp_down_texture_, false, x, y, width, height, screen_width,
                  screen_height, r, g, b, alpha);
    }
}

unsigned int OsdFont::create_mask(const unsigned char* coverage, int width, int height) {
    if (!coverage || width <= 0 || height <= 0) {
        return 0;
    }
    const GLuint texture = upload_texture(GL_ALPHA, width, height, coverage);
    images_.push_back(texture);
    return texture;
}

void OsdFont::draw_mask(unsigned int texture, int x, int y, int width, int height, int screen_width,
                        int screen_height, float r, float g, float b, float alpha) {
    if (texture && ensure_program()) {
        draw_quad(texture, false, x, y, width, height, screen_width, screen_height, r, g, b, alpha);
    }
}

unsigned int OsdFont::create_image(const unsigned char* rgba, int width, int height) {
    if (!rgba || width <= 0 || height <= 0) {
        return 0;
    }
    const GLuint texture = upload_texture(GL_RGBA, width, height, rgba);
    images_.push_back(texture);
    return texture;
}

void OsdFont::draw_image(unsigned int texture, int x, int y, int width, int height, int screen_width,
                         int screen_height) {
    if (texture && ensure_program()) {
        draw_quad(texture, true, x, y, width, height, screen_width, screen_height, 1.0f, 1.0f, 1.0f, 1.0f);
    }
}

void OsdFont::draw_quad(unsigned int texture, bool picture, int x, int y, int width, int height,
                        int screen_width, int screen_height, float r, float g, float b, float alpha) {
    // mpv renders into the same context: everything changed here goes back afterwards, and
    // the quad comes from our own buffer (and vertex array when there is one) rather than
    // client memory. Drawing from client memory left black stripes and black frames in the
    // video in the desktop tests.
    GLint saved_active_texture = GL_TEXTURE0;
    GLint saved_texture = 0;
    GLint saved_program = 0;
    GLint saved_array_buffer = 0;
    GLint saved_vertex_array = 0;
    GLint saved_blend_src_rgb = GL_ONE;
    GLint saved_blend_dst_rgb = GL_ZERO;
    GLint saved_blend_src_alpha = GL_ONE;
    GLint saved_blend_dst_alpha = GL_ZERO;
    GLint saved_pos_enabled = 0;
    GLint saved_uv_enabled = 0;
    const GLboolean saved_blend = glIsEnabled(GL_BLEND);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &saved_active_texture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &saved_texture);
    glGetIntegerv(GL_CURRENT_PROGRAM, &saved_program);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &saved_array_buffer);
    glGetIntegerv(GL_BLEND_SRC_RGB, &saved_blend_src_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &saved_blend_dst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &saved_blend_src_alpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &saved_blend_dst_alpha);
    if (vertex_array_) {
        glGetIntegerv(kVertexArrayBinding, &saved_vertex_array);
    } else {
        glGetVertexAttribiv(attr_pos_, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &saved_pos_enabled);
        glGetVertexAttribiv(attr_uv_, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &saved_uv_enabled);
    }

    const float x0 = 2.0f * x / screen_width - 1.0f;
    const float x1 = 2.0f * (x + width) / screen_width - 1.0f;
    const float y0 = 1.0f - 2.0f * y / screen_height;
    const float y1 = 1.0f - 2.0f * (y + height) / screen_height;
    const GLfloat vertices[] = {
        x0, y0, 0.0f, 0.0f,
        x1, y0, 1.0f, 0.0f,
        x0, y1, 0.0f, 1.0f,
        x1, y1, 1.0f, 1.0f,
    };
    if (vertex_array_) {
        bind_vertex_array_(vertex_array_);
    }
    glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STREAM_DRAW);
    if (!vertex_array_) {
        set_attributes();
    }
    glViewport(0, 0, screen_width, screen_height);
    glUseProgram(program_);
    glBindTexture(GL_TEXTURE_2D, texture);
    glUniform1i(uniform_texture_, 0);
    glUniform3f(uniform_color_, r, g, b);
    glUniform1f(uniform_picture_, picture ? 1.0f : 0.0f);
    glUniform1f(uniform_alpha_, alpha);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    if (vertex_array_) {
        bind_vertex_array_(saved_vertex_array);
    } else {
        if (!saved_pos_enabled) {
            glDisableVertexAttribArray(attr_pos_);
        }
        if (!saved_uv_enabled) {
            glDisableVertexAttribArray(attr_uv_);
        }
    }
    glBlendFuncSeparate(saved_blend_src_rgb, saved_blend_dst_rgb, saved_blend_src_alpha, saved_blend_dst_alpha);
    if (!saved_blend) {
        glDisable(GL_BLEND);
    }
    glUseProgram(saved_program);
    glBindBuffer(GL_ARRAY_BUFFER, saved_array_buffer);
    glBindTexture(GL_TEXTURE_2D, saved_texture);
    glActiveTexture(saved_active_texture);
}

void OsdFont::set_attributes() {
    glVertexAttribPointer(attr_pos_, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), nullptr);
    glVertexAttribPointer(attr_uv_, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
                          reinterpret_cast<const void*>(2 * sizeof(GLfloat)));
    glEnableVertexAttribArray(attr_pos_);
    glEnableVertexAttribArray(attr_uv_);
}

void OsdFont::release() {
    for (auto& [key, text] : cache_) {
        glDeleteTextures(1, &text.texture);
    }
    cache_.clear();
    if (!images_.empty()) {
        glDeleteTextures(static_cast<GLsizei>(images_.size()), images_.data());
        images_.clear();
    }
    for (unsigned int* texture : {&solid_texture_, &ramp_down_texture_, &ramp_up_texture_}) {
        if (*texture) {
            glDeleteTextures(1, texture);
            *texture = 0;
        }
    }
    if (program_) {
        glDeleteProgram(program_);
        program_ = 0;
    }
    if (vertex_buffer_) {
        glDeleteBuffers(1, &vertex_buffer_);
        vertex_buffer_ = 0;
    }
    if (vertex_array_ && delete_vertex_arrays_) {
        delete_vertex_arrays_(1, &vertex_array_);
    }
    vertex_array_ = 0;
}

std::vector<unsigned char> decode_picture(const std::string& data, int& width, int& height) {
    int channels = 0;
    unsigned char* pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(data.data()),
                                                  static_cast<int>(data.size()), &width, &height, &channels, 4);
    if (!pixels) {
        width = 0;
        height = 0;
        return {};
    }
    std::vector<unsigned char> rgba(pixels, pixels + static_cast<size_t>(width) * height * 4);
    stbi_image_free(pixels);
    return rgba;
}

}  // namespace newpipe
