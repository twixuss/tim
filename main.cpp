#pragma warning(disable: 4996)
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#undef assert

#define TL_DEBUG 0

#define TL_IMPL
#include <tl/string.h>
#include <tl/file.h>
#include <tl/console.h>
#include <tl/precise_time.h>
#include <tl/main.h>
#include <tl/qoi.h>

using namespace tl;

using String = Span<utf8>;

inline constexpr bool operator==(String a, char const *b) { return a == as_utf8(as_span(b)); }

using Pixel = v3u8;

struct Image {
    Pixel *pixels = 0;
    v2u size = {};

    Pixel &operator()(s32 x, s32 y) {
        assert((u32)x < size.x && (u32)y <= size.y);
        return pixels[y*size.x + x];
    }
};

struct BitSerializer {
    u8 *buffer = 0;
    u8 *cursor = 0;
    u8 shift = 0;
    
    void write_1_bit(u8 value) {
        *cursor &= ~(1 << shift);
        *cursor |= (value & 1) << shift;
        shift += 1;
        if (shift == 8) {
            shift = 0;
            cursor++;
        }
    }
    template <umm count>
    void write_bits(u64 value) {
        for (int i = 0; i < count; ++i) {
            write_1_bit((value >> i) & 1);
        }
    }
    template <umm count>
    void write_bits_be(u64 value) {
        for (int i = count - 1; i >= 0; --i) {
            write_1_bit((value >> i) & 1);
        }
    }

    u8 read_1_bit() {
        u8 result = (*cursor >> shift) & 1;
        shift += 1;
        if (shift == 8) {
            shift = 0;
            cursor++;
        }
        return result;
    }
    template <umm count>
    u64 read_bits() {
        u64 result = 0;
        for (int i = 0; i < count; ++i) {
            result |= ((u64)read_1_bit() << i);
        }
        return result;
    }
    template <umm count>
    u64 read_bits_be() {
        u64 result = 0;
        for (int i = count - 1; i >= 0; --i) {
            result |= ((u64)read_1_bit() << i);
        }
        return result;
    }

    Span<u8> span() {
        if (shift) {
            return {buffer, cursor + 1};
        } else {
            return {buffer, cursor};
        }
    }
};

umm uncompressed_size = 0;
umm n_bits0 = 0;
umm n_bits1 = 0;
umm n_bits2 = 0;
umm n_bits4 = 0;
umm n_bits8 = 0;

#define profile()                                                                    \
    auto timer = create_precise_timer();                                             \
    defer {                                                                          \
        auto time = elapsed_time(timer);                                             \
        println("{}ms / {}/s", time * 1000, format_bytes(uncompressed_size / time)); \
    };

void encode_mip(BitSerializer &serializer, Image image) {
    Pixel *mids = DefaultAllocator{}.allocate<Pixel>(image.size.x * image.size.y / 4);
    for (int component = 0; component < 3; ++component) {
        for (umm y = 0; y < image.size.y / 2; ++y) {
        for (umm x = 0; x < image.size.x / 2; ++x) {
            auto &p1 = image(x*2 + 0, y*2 + 0).s[component];
            auto &p2 = image(x*2 + 1, y*2 + 0).s[component];
            auto &p3 = image(x*2 + 0, y*2 + 1).s[component];
            auto &p4 = image(x*2 + 1, y*2 + 1).s[component];

            auto mn = min(p1, p2, p3, p4);
            auto mx = max(p1, p2, p3, p4);

            mids[y*image.size.x/2 + x].s[component] = (u8)(((u16)mn + (u16)mx + 1) / 2);
        }
        }
    }

    if (image.size.x == 2) {
        serializer.write_bits<8>(mids[0].x);
        serializer.write_bits<8>(mids[0].y);
        serializer.write_bits<8>(mids[0].z);
    } else {
        encode_mip(serializer, {mids, image.size / 2});
    }

    for (int component = 0; component < 3; ++component) {
        for (smm y = 0; y < image.size.y / 2; ++y) {
        for (smm x = 0; x < image.size.x / 2; ++x) {
            auto &p1 = image(x*2 + 0, y*2 + 0).s[component];
            auto &p2 = image(x*2 + 1, y*2 + 0).s[component];
            auto &p3 = image(x*2 + 0, y*2 + 1).s[component];
            auto &p4 = image(x*2 + 1, y*2 + 1).s[component];

            u8 mid = mids[y*image.size.x/2 + x].s[component];

            s8 deltas[4];
            deltas[0] = p1 - mid;
            deltas[1] = p2 - mid;
            deltas[2] = p3 - mid;
            deltas[3] = p4 - mid;
    
            u8 bits_required = 0;
            for (auto delta : deltas) {
                if (delta == 0) {
                    // no bits required
                } else if (-1 <= delta && delta < 1) {
                    bits_required = max(bits_required, (u8)1);
                } else if (-2 <= delta && delta < 2) {
                    bits_required = max(bits_required, (u8)2);
                } else if (-8 <= delta && delta < 8) {
                    bits_required = max(bits_required, (u8)4);
                } else {
                    bits_required = max(bits_required, (u8)8);
                    break;
                }
            }

            switch (bits_required) {
                case 0: {
                    ++n_bits0;
                    serializer.write_bits_be<3>(0b011);
                    break;
                }
                case 1: {
                    ++n_bits1;
                    serializer.write_bits_be<2>(0b11);
                    for (int i = 0; i < 4; ++i)
                        serializer.write_bits<1>(deltas[i]);
                    break;
                }
                case 2: {
                    ++n_bits2;
                    serializer.write_bits_be<2>(0b10);
                    for (int i = 0; i < 4; ++i)
                        serializer.write_bits<2>(deltas[i]);
                    break;
                }
                case 4: {
                    ++n_bits4;
                    serializer.write_bits_be<2>(0b00);
                    for (int i = 0; i < 4; ++i)
                        serializer.write_bits<4>(deltas[i]);
                    break;
                }
                case 8: {
                    ++n_bits8;
                    serializer.write_bits_be<3>(0b010);
                    for (int i = 0; i < 4; ++i)
                        serializer.write_bits<8>(deltas[i]);
                    break;
                }
                default: invalid_code_path();
            }
        }
        }
    }
}

void encode(BitSerializer &serializer, Image in) {
    profile();

    serializer.write_bits<32>(in.size.x);
    serializer.write_bits<32>(in.size.y);

    encode_mip(serializer, in);
}

Pixel *decode_mip(BitSerializer &serializer, v2u size, Pixel *prev_mip) {
    Pixel *mip = DefaultAllocator{}.allocate<Pixel>(size.x * size.y);

    return mip;
}

Image decode(BitSerializer &serializer) {
    Image out = {};
    out.size.x = serializer.read_bits<32>();
    out.size.y = serializer.read_bits<32>();
    
    uncompressed_size = out.size.x * out.size.y * sizeof(Pixel);

    profile();

    auto buffer_in  = DefaultAllocator{}.allocate<Pixel>(out.size.x * out.size.y);
    auto buffer_out = DefaultAllocator{}.allocate<Pixel>(out.size.x * out.size.y);

    buffer_out[0].s[0] = serializer.read_bits<8>();
    buffer_out[0].s[1] = serializer.read_bits<8>();
    buffer_out[0].s[2] = serializer.read_bits<8>();
    
    umm mip_count = log2(out.size.x) + 1;
    for (umm i = 1; i < mip_count; ++i) {
        auto mip_size = V2u(1 << i);

        Swap(buffer_in, buffer_out);

        for (int component = 0; component < 3; ++component) {
            for (smm y = 0; y < mip_size.y / 2; ++y) {
            for (smm x = 0; x < mip_size.x / 2; ++x) {
                u8 mid = buffer_in[y * out.size.x + x].s[component];
                s8 deltas[4];

                if (serializer.read_bits<1>() == 1) {
                    if (serializer.read_bits<1>() == 1) {
                        struct X { s8 value : 1; } x;
                        for (int i = 0; i < 4; ++i) {
                            deltas[i] = x.value = serializer.read_bits<1>();
                        }
                    } else {
                        struct X { s8 value : 2; } x;
                        for (int i = 0; i < 4; ++i) {
                            deltas[i] = x.value = serializer.read_bits<2>();
                        }
                    }
                } else if (serializer.read_bits<1>() == 1) {
                    if (serializer.read_bits<1>() == 1) {
                        for (int i = 0; i < 4; ++i) {
                            deltas[i] = 0;
                        }
                    } else {
                        for (int i = 0; i < 4; ++i) {
                            deltas[i] = serializer.read_bits<8>();
                        }
                    }
                } else {
                    struct X { s8 value : 4; } x;
                    for (int i = 0; i < 4; ++i) {
                        deltas[i] = x.value = serializer.read_bits<4>();
                    }
                }
                
                buffer_out[(y*2 + 0) * out.size.x + (x*2 + 0)].s[component] = deltas[0] + mid;
                buffer_out[(y*2 + 0) * out.size.x + (x*2 + 1)].s[component] = deltas[1] + mid;
                buffer_out[(y*2 + 1) * out.size.x + (x*2 + 0)].s[component] = deltas[2] + mid;
                buffer_out[(y*2 + 1) * out.size.x + (x*2 + 1)].s[component] = deltas[3] + mid;
            }
            }
        }
    }
    
    out.pixels = buffer_out;

    return out;
}

void usage() {
    println("Usage: tim [encode(default)|decode] <input> -o <output>");
    println("  encode");
    println("    Convert image to tim");
    println("  decode");
    println("    Convert tim to other format");
}

s32 tl_main(Span<String> args) {
    if (args.count == 1) {
        usage();
        return 1;
    }
    String input_path;
    String output_path;

    bool should_encode = true;
    bool bench = false;

    for (umm i = 0; i < args.count; ++i) {
        if (args[i] == "help") {
            usage();
        } else if (args[i] == "-o") {
            ++i;
            if (i >= args.count) {
                println("Expected an output path after -o");
                return 1;
            }
            output_path = args[i];
        } else if (args[i] == "encode") {
            should_encode = true;
        } else if (args[i] == "decode") {
            should_encode = false;
        } else if (args[i] == "bench") {
            bench = true;
        } else {
            input_path = args[i];
        }
    }

    if (!input_path) {
        println("Provide a path to input image");
        return 1;
    }

    if (!output_path) {
        if (should_encode) {
            output_path = format(u8"{}.tim", parse_path(input_path).path_without_extension());
        } else {
            println("Provide a path to output image using -o flag. I need to know the extension.");
            return 1;
        }
    }

    
    if (should_encode) {
        println("encoding");

        Image in;
        in.pixels = autocast stbi_load(autocast null_terminate(input_path).data, autocast &in.size.x, autocast &in.size.y, 0, 3);

        if (in.size.x == in.size.y && is_power_of_2(in.size.x)) {
            // ok
        } else {
            println("Only power of two square images are supported currently");
            return 1;
        }
    
        uncompressed_size = in.size.x * in.size.y * sizeof(Pixel);
        
        BitSerializer serializer;
        constexpr u32 times_more_that_uncompressed = 2; // should be big enough
        serializer.buffer = DefaultAllocator{}.allocate<u8>(uncompressed_size * times_more_that_uncompressed);
        serializer.cursor = serializer.buffer;

           
        encode(serializer, in);


        auto n_bits_sum = n_bits0 + n_bits1 + n_bits2 + n_bits4 + n_bits8;

        println("Compressed size: {} ({}% of uncompressed)", format_bytes(serializer.span().count), serializer.span().count * 100.0f / uncompressed_size);
        println("    n_bits0: {} ({}%)", n_bits0, n_bits0 * 100.0f / n_bits_sum);
        println("    n_bits1: {} ({}%)", n_bits1, n_bits1 * 100.0f / n_bits_sum);
        println("    n_bits2: {} ({}%)", n_bits2, n_bits2 * 100.0f / n_bits_sum);
        println("    n_bits4: {} ({}%)", n_bits4, n_bits4 * 100.0f / n_bits_sum);
        println("    n_bits8: {} ({}%)", n_bits8, n_bits8 * 100.0f / n_bits_sum);

        write_entire_file(output_path, serializer.span());
    } else {
        println("decoding");

        BitSerializer serializer;
        serializer.buffer = read_entire_file(input_path).data;
        serializer.cursor = serializer.buffer;


        Image out = decode(serializer);
        
        if (ends_with(output_path, u8".bmp"s)) {
            stbi_write_bmp(autocast null_terminate(output_path).data, out.size.x, out.size.y, 3, out.pixels);
        } else if (ends_with(output_path, u8".png"s)) {
            stbi_write_png(autocast null_terminate(output_path).data, out.size.x, out.size.y, 3, out.pixels, out.size.x * sizeof(Pixel));
        } else if (ends_with(output_path, u8".jpg"s)) {
            stbi_write_jpg(autocast null_terminate(output_path).data, out.size.x, out.size.y, 3, out.pixels, 100);
        } else if (ends_with(output_path, u8".tga"s)) {
            stbi_write_tga(autocast null_terminate(output_path).data, out.size.x, out.size.y, 3, out.pixels);
        } else if (ends_with(output_path, u8".qoi"s)) {
            write_entire_file(output_path, qoi::encode(out.pixels, out.size));
        } else {
            println("Unknown output extension");
            return 1;
        }
    }

    if (bench) {
        void *pixels;
        int x,y;
        println("bmp");{profile();pixels = stbi_load("tests/test.bmp", &x, &y, 0, 3);}{profile();stbi_write_bmp("tests/garbage.bmp", 4096, 4096, 3, pixels);}
        println("png");{profile();         stbi_load("tests/test.png", &x, &y, 0, 3);}{profile();stbi_write_png("tests/garbage.png", 4096, 4096, 3, pixels, 4096*3);}
        println("jpg");{profile();         stbi_load("tests/test.jpg", &x, &y, 0, 3);}{profile();stbi_write_jpg("tests/garbage.jpg", 4096, 4096, 3, pixels, 100);}
        println("tga");{profile();         stbi_load("tests/test.tga", &x, &y, 0, 3);}{profile();stbi_write_tga("tests/garbage.tga", 4096, 4096, 3, pixels);}
        println("qoi");{profile();         qoi::decode(read_entire_file("tests/test.qoi"s));}{profile();qoi::encode((v3u8 *)pixels, v2u{4096, 4096}); }

        umm size = 0;
        size = get_file_size("tests/test.bmp"s).value_or(0); println("bmp {} {}", format_bytes(size), size * 100.0f / uncompressed_size);
        size = get_file_size("tests/test.png"s).value_or(0); println("png {} {}", format_bytes(size), size * 100.0f / uncompressed_size);
        size = get_file_size("tests/test.jpg"s).value_or(0); println("jpg {} {}", format_bytes(size), size * 100.0f / uncompressed_size);
        size = get_file_size("tests/test.tga"s).value_or(0); println("tga {} {}", format_bytes(size), size * 100.0f / uncompressed_size);
        size = get_file_size("tests/test.qoi"s).value_or(0); println("qoi {} {}", format_bytes(size), size * 100.0f / uncompressed_size);
    }

    return 0;
}
