#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace newpipe {

// Text for the player's own overlay (info bar, loading screen, countdown). A TrueType font is
// rasterized with stb_truetype into one GL texture per string. The Switch system font has the
// Turkish letters the old 5x7 bitmap font lacked. Use it only on the GL thread, between
// load() and release().
class OsdFont {
public:
    OsdFont();
    ~OsdFont();

    // Switch: the shared Standard font; desktop: resources/font/switch_font.ttf or DejaVu Sans.
    bool load();
    bool ready() const { return ready_; }
    int measure(const std::string& utf8, int pixel_height);
    // (x, y) is the top-left corner in window pixels, y growing downwards.
    void draw(const std::string& utf8, int x, int y, int pixel_height, int screen_width, int screen_height,
              float r, float g, float b);
    // Pictures (the next video's thumbnail): RGBA pixels become a texture that draw_image()
    // scales into a window rectangle. release() frees them with the text textures.
    unsigned int create_image(const unsigned char* rgba, int width, int height);
    void draw_image(unsigned int texture, int x, int y, int width, int height, int screen_width, int screen_height);
    // Shapes as coverage (one byte per pixel), drawn in any color and opacity.
    unsigned int create_mask(const unsigned char* coverage, int width, int height);
    void draw_mask(unsigned int texture, int x, int y, int width, int height, int screen_width, int screen_height,
                   float r, float g, float b, float alpha);
    // Translucent rectangles over the video: fill() in one opacity; fade() from alpha at one
    // edge (the top when darker_at_top) to clear at the other.
    void fill(int x, int y, int width, int height, int screen_width, int screen_height, float r, float g, float b,
              float alpha);
    void fade(int x, int y, int width, int height, int screen_width, int screen_height, float r, float g, float b,
              float alpha, bool darker_at_top);
    // Frees the GL objects; call while the context is still current.
    void release();

private:
    struct Text {
        unsigned int texture = 0;
        int width = 0;
        int height = 0;
        int overhang = 0;  // pixels the texture reaches above the line's top (accents on capitals)
        uint64_t last_used = 0;
    };

    const Text* text_for(const std::string& utf8, int pixel_height);
    bool ensure_program();
    void set_attributes();
    void evict();
    // One textured quad; picture: the texture's own colors, else its alpha in (r, g, b); all
    // of it at opacity alpha.
    void draw_quad(unsigned int texture, bool picture, int x, int y, int width, int height, int screen_width,
                   int screen_height, float r, float g, float b, float alpha);

    struct FontInfo;
    FontInfo* font_ = nullptr;
    std::vector<unsigned char> file_data_;  // desktop: the font file (the Switch font lives in shared memory)
    bool ready_ = false;
    unsigned int program_ = 0;
    unsigned int vertex_buffer_ = 0;
    unsigned int vertex_array_ = 0;  // 0 when the context has no vertex arrays
    void (*bind_vertex_array_)(unsigned int) = nullptr;
    void (*delete_vertex_arrays_)(int, const unsigned int*) = nullptr;
    int attr_pos_ = -1;
    int attr_uv_ = -1;
    int uniform_color_ = -1;
    int uniform_texture_ = -1;
    int uniform_picture_ = -1;
    int uniform_alpha_ = -1;
    unsigned int solid_texture_ = 0;      // one opaque texel, for fill()
    unsigned int ramp_down_texture_ = 0;  // clear at the top, opaque at the bottom, for fade()
    unsigned int ramp_up_texture_ = 0;    // the other way round
    uint64_t frame_ = 0;
    std::unordered_map<std::string, Text> cache_;
    std::vector<unsigned int> images_;
};

// A JPEG or PNG file as RGBA pixels (empty when it cannot be read). Safe on any thread.
std::vector<unsigned char> decode_picture(const std::string& data, int& width, int& height);

}  // namespace newpipe
