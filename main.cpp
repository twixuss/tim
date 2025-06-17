#define TL_IMPL
#include <tl/string.h>
#include <tl/file.h>
#include <tl/console.h>
#include <tl/precise_time.h>

#pragma warning(disable: 4996)
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#include <assert.h>

using namespace tl;


struct Pixel {
    u8 r, g, b;
};

int totalR;
int totalG;
int totalB;
int totalN;
int totalZero;
int totalSmall;
int totalFull;

#define COUNTERS 0

int n0;
int n2;
int n4;
int n8;
int histogram[256];

struct Endecoder {
    u8 *r_start = 0;
    u8 *g_start = 0;
    u8 *b_start = 0;

    struct Channel {
        u8 *p = 0;
        u8 bit : 3 = 0;
    };

    Channel r, g, b;

    u32 size = 0;

public:
    static Endecoder encoder(u32 w) {
        Endecoder result;
        result.size = w;
        result.r.p = result.r_start = (u8 *)malloc(w*w*2);
        result.g.p = result.g_start = (u8 *)malloc(w*w*2);
        result.b.p = result.b_start = (u8 *)malloc(w*w*2);
        return result;
    }
    static Endecoder decoder(Span<char> path) {
        Endecoder result;
        auto data = read_entire_file(path);
        auto p = data.data;

        result.size = *(u32 *)p;
        p += sizeof(u32);

        auto r_size = *(u32 *)p;
        p += sizeof(u32);

        auto g_size = *(u32 *)p;
        p += sizeof(u32);

        result.r.p = p;
        p += r_size;

        result.g.p = p;
        p += g_size;

        result.b.p = p;

        return result;
    }
    void write(Pixel pixel) {
        write_channel(r, pixel.r);
        write_channel(g, pixel.g);
        write_channel(b, pixel.b);
    }
    Pixel read() {
        return {
            read_channel(r),
            read_channel(g),
            read_channel(b),
        };
    }
    void save(Span<ascii> path) {
        auto file = open_file(path, {.write = true});
        defer { close(file); };

        ::write(file, value_as_bytes(size));
        ::write(file, value_as_bytes((u32)(r.p - r_start)));
        ::write(file, value_as_bytes((u32)(g.p - g_start)));
        ::write(file, Span(r_start, r.p));
        ::write(file, Span(g_start, g.p));
        ::write(file, Span(b_start, b.p));
    }
private:
    void write_2bits(Channel &c, u8 v) {
        u8 masks[4] {
            0b11111100,
            0b11110011,
            0b11001111,
            0b00111111,
        };

        *c.p &= masks[c.bit >> 1];
        *c.p |= (v & 3) << c.bit;

        c.bit += 2;
        c.p += c.bit == 0;

    }
    u8 read_2bits(Channel &c) {
        u8 masks[4] {
            0b00000011,
            0b00001100,
            0b00110000,
            0b11000000,
        };

        defer {
            c.bit += 2;
            c.p += c.bit == 0;
        };
        return (*c.p & masks[c.bit >> 1]) >> c.bit;
    }

    void write_channel(Channel &c, u8 v) {
        auto zeros = count_leading_zeros(v);
        auto ones  = count_leading_ones (v);
        auto leading = max(ones, zeros);

        // max 2-bit number we can save is 0b(000000)01 (1)
        // min 2-bit number we can save is 0b(111111)10 (-2)

        // max 4-bit number we can save is 0b(0000)0111 (7)
        // min 4-bit number we can save is 0b(1111)1000 (-8)

#if COUNTERS
        histogram[v]++;
#endif

        if (v == 0) {
            write_2bits(c, 0);
#if COUNTERS
            ++n0;
#endif
        } else if (leading >= 7) {
            write_2bits(c, 1);
            write_2bits(c, v);
#if COUNTERS
            ++n2;
#endif
        } else if (leading >= 5) {
            write_2bits(c, 2);
            write_2bits(c, v);
            write_2bits(c, v >> 2);
#if COUNTERS
            ++n4;
#endif
        } else {
            write_2bits(c, 3);
            write_2bits(c, v);
            write_2bits(c, v >> 2);
            write_2bits(c, v >> 4);
            write_2bits(c, v >> 6);
#if COUNTERS
            ++n8;
#endif
        }
    }
    u8 read_channel(Channel &c) {
        switch (read_2bits(c)) {
            case 0: return 0;
            case 1: {
                u8 v = read_2bits(c);
                v |= v & 0b00000010 ? 0b11111100 : 0;
                return v;
            }
            case 2: {
                u8 v = read_2bits(c);
                v |= read_2bits(c) << 2;
                v |= v & 0b00001000 ? 0b11110000 : 0;
                return v;
            }
            case 3: {
                u8 v = read_2bits(c);
                v |= read_2bits(c) << 2;
                v |= read_2bits(c) << 4;
                v |= read_2bits(c) << 6;
                return v;
            }
        }
    }

};

u32 const remove_bits = 0;

void decode() {
    auto decoder = Endecoder::decoder("result.tim"s);

    auto w = decoder.size, h = w;
    auto pixels = new Pixel[w*w];

    {
        auto layer = decoder.read();
        for (int y = 0; y < w; ++y) {
            for (int x = 0; x < w; ++x) {
                pixels[y*w + x] = layer;
            }
        }
    }
     //stbi_write_bmp("tmp/decoded1.bmp", w, h, 3, pixels);

    for (int s = 2; s <= w; s *= 2) {

        for (int py = 0; py < s; ++py) {
            for (int px = 0; px < s; ++px) {

                auto layer = decoder.read();
                for (int y = h*py/s; y < h*(py+1)/s; ++y) {
                    for (int x = w*px/s; x < w*(px+1)/s; ++x) {
                        auto &pixel = pixels[y*w + x];
                        pixel.r += layer.r;
                        pixel.g += layer.g;
                        pixel.b += layer.b;
                    }
                }

            }
        }

        char pathbuf[256];
        sprintf(pathbuf, "tmp/decoded%d.bmp", s);
        //stbi_write_bmp(pathbuf, w, h, 3, pixels);
    }


    for (int y = 0; y < w; ++y) {
        for (int x = 0; x < w; ++x) {
            auto &pixel = pixels[y*w + x];
            pixel.r <<= remove_bits;
            pixel.g <<= remove_bits;
            pixel.b <<= remove_bits;
        }
    }


    stbi_write_bmp("decoded.bmp", w, h, 3, pixels);
}

void encode() {
    int w, h;
    auto pixels = (Pixel *)stbi_load("test.png", &w, &h, 0, 3);
    assert(w == h && _mm_popcnt_u32(w) == 1);

    auto writer = Endecoder::encoder(w);

    for (int y = 0; y < w; ++y) {
        for (int x = 0; x < w; ++x) {
            auto &pixel = pixels[y*w + x];
            pixel.r >>= remove_bits;
            pixel.g >>= remove_bits;
            pixel.b >>= remove_bits;
        }
    }

    {
        Pixel mn = {255,255,255};
        Pixel mx = {0,0,0};

        for (int y = 0; y < w; ++y) {
            for (int x = 0; x < w; ++x) {
                auto pixel = pixels[y*w + x];
                mn.r = min(mn.r, pixel.r);
                mn.g = min(mn.g, pixel.g);
                mn.b = min(mn.b, pixel.b);
                mx.r = max(mx.r, pixel.r);
                mx.g = max(mx.g, pixel.g);
                mx.b = max(mx.b, pixel.b);
            }
        }

        Pixel layer {
            mn.r + (mx.r - mn.r) / 2,
            mn.g + (mx.g - mn.g) / 2,
            mn.b + (mx.b - mn.b) / 2,
        };

        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                auto &pixel = pixels[y*w + x];
                pixel.r -= layer.r;
                pixel.g -= layer.g;
                pixel.b -= layer.b;
            }
        }
        //stbi_write_bmp("tmp/avg1.bmp", 1, 1, 3, &layer);

        writer.write(layer);
    }

    for (int s = 2; s <= w; s *= 2) {
        auto layer_pixels = new Pixel[s*s];
        for (int py = 0; py < s; ++py) {
            for (int px = 0; px < s; ++px) {
                Pixel mn = {127,127,127};
                Pixel mx = {-128,-128,-128};
                for (int y = h*py/s; y < h*(py+1)/s; ++y) {
                    for (int x = w*px/s; x < w*(px+1)/s; ++x) {
                        auto pixel = pixels[y*w + x];
                        mn.r = min((s8)mn.r, (s8)pixel.r);
                        mn.g = min((s8)mn.g, (s8)pixel.g);
                        mn.b = min((s8)mn.b, (s8)pixel.b);
                        mx.r = max((s8)mx.r, (s8)pixel.r);
                        mx.g = max((s8)mx.g, (s8)pixel.g);
                        mx.b = max((s8)mx.b, (s8)pixel.b);
                    }
                }
                Pixel layer {
                    mn.r + ((s8)(mx.r - mn.r) >> 1),
                    mn.g + ((s8)(mx.g - mn.g) >> 1),
                    mn.b + ((s8)(mx.b - mn.b) >> 1),
                };
                for (int y = h*py/s; y < h*(py+1)/s; ++y) {
                    for (int x = w*px/s; x < w*(px+1)/s; ++x) {
                        auto &pixel = pixels[y*w + x];
                        pixel.r -= layer.r;
                        pixel.g -= layer.g;
                        pixel.b -= layer.b;
                    }
                }

                layer_pixels[py*s + px] = layer;

                // printf("%d: [%d, %d]: min: {%d, %d, %d}, max: {%d, %d, %d}\n", s, px, py, mn.r, mn.g, mn.b, mx.r, mx.g, mx.b);
                writer.write(layer);
            }
        }

        char pathbuf[256];


        for (int y = 0; y < s; ++y) {
            for (int x = 0; x < s; ++x) {
                auto &pixel = layer_pixels[y*s + x];
                pixel.r += 128;
                pixel.g += 128;
                pixel.b += 128;
            }
        }
        sprintf(pathbuf, "tmp/avg%d.bmp", s);
        //stbi_write_bmp(pathbuf, s, s, 3, layer_pixels);
        for (int y = 0; y < s; ++y) {
            for (int x = 0; x < s; ++x) {
                auto &pixel = layer_pixels[y*s + x];
                pixel.r -= 128;
                pixel.g -= 128;
                pixel.b -= 128;
            }
        }



        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                auto &pixel = pixels[y*w + x];
                pixel.r += 128;
                pixel.g += 128;
                pixel.b += 128;
            }
        }
        sprintf(pathbuf, "tmp/layer%d.bmp", s);
        //stbi_write_bmp(pathbuf, w, h, 3, pixels);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                auto &pixel = pixels[y*w + x];
                pixel.r -= 128;
                pixel.g -= 128;
                pixel.b -= 128;
            }
        }
    }
    writer.save("result.tim"s);
}

int main() {
    init_allocator();
    init_printer();

    /*
         0  1  2  3  4  5  6  7  min = 0
         8  9 10 11 12 13 14 15  max = 63
        16 17 18 19 20 21 22 23  avg = 0 + (63 - 0)/2 = 31
        24 25 26 27 28 29 30 31
        32 33 34 35 36 37 38 39
        40 41 42 43 44 45 46 47
        48 49 50 51 52 53 54 55
        56 57 58 59 60 61 62 63

        store avg.

        -31 -30 -29 -28 min = -31 |  -27 -26 -25 -24 min = ...
        -23 -22 -21 -20 max = -4  |  -19 -18 -17 -16 max = ...
        -15 -14 -13 -12 avg = -18 |  -11 -10  -9  -8 avg = ...
         -7  -6  -5  -4           |   -3  -2  -1   0
        ==========================|=================
          1   2   3   4 min = ... |    5   6   7   8 min = ...
          9  10  11  12 max = ... |   13  14  15  16 max = ...
         17  18  19  20 avg = ... |   21  22  23  24 avg = ...
         25  26  27  28           |   29  30  31  32

         -13 -12  | -11 -10  |  -9  -8 | -7  -6
          -5  -4  |  -3  -2  |  -1   0 |  1   2
        ==========|==========|=========|=======
           3   4  |   5   6  |   7   8 |  9  10
          11  12  |  13  14  |  15  16 | 17  18
        ==========|==========|=========|=======
          19  20  |  21  22  |  23  24 | 25  26
        ==========|==========|=========|=======
          27  28  |  29  30  |  31  32 | 33  34
          35  36  |  37  38  |  39  40 | 41  42
          43  44  |  45  46  |  47  48 | 49  50
    */


    //for (int y = 0; y < 8; ++y) {
    //    for (int x = 0; x < 8; ++x) {
    //        printf("%3d ", y*8 + x - 31 + 18);
    //    }
    //    printf("\n");
    //}

    {
        auto timer = create_precise_timer();
        defer { print("Encoding: {} ms\n", elapsed_time(timer) * 1000); };

        encode();
    }

    // print("Average written pixel: ({}, {}, {})\n", totalR/totalN, totalG/totalN, totalB/totalN);
    print("n0: {}\nn2: {}\nn4: {}\nn8: {}\n", n0, n2, n4, n8);
    for (int i = 0; i < 256; ++i) {
        print("{}: {}\n", i, histogram[i]);
    }

    {
        auto timer = create_precise_timer();
        defer { print("Decoding: {} ms\n", elapsed_time(timer) * 1000); };

        decode();
    }
}
