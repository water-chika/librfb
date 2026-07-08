#pragma once

#include "zlib.h"

#include "utility.hpp"
#include <span>
#include <format>

namespace rfb {

template<typename T>
class add_zrle : public T {
public:
    using parent = T;
    add_zrle(const configure auto& conf) : parent{conf},
        zlib_stream{}
    {
        int ret = inflateInit(&zlib_stream);
        if (ret != Z_OK) {
            throw std::runtime_error("deflateInit error");
        }
    }
    ~add_zrle() {
        inflateEnd(&zlib_stream);
    }
    uint32_t cpixel_to_pixel(std::span<uint8_t> c) {
        return c[0] | (c[1]<<(1*8)) | (c[2]<<(2*8));
    }
    void zrle_decode(std::span<uint8_t> zlib_data, int sx, int sy, int width, int height, std::span<uint8_t> frame_u8) {
        constexpr int CHUNK = 1024;

        auto frame = reinterpret_cast<uint32_t*>(frame_u8.data());

        zlib_stream.avail_in = zlib_data.size();
        zlib_stream.next_in = zlib_data.data();

        std::vector<uint8_t> data{};

        do {
            auto data_previous_size = data.size();
            data.resize(data.size() + CHUNK);
            zlib_stream.avail_out = CHUNK;
            zlib_stream.next_out = data.data() + data_previous_size;

            auto ret = inflate(&zlib_stream, Z_NO_FLUSH);
            if (ret == Z_STREAM_ERROR) {
                throw std::runtime_error{"zlib deflate error"};
            }
            auto have = CHUNK - zlib_stream.avail_out;
        } while (zlib_stream.avail_out == 0);
        data.resize(data.size() - zlib_stream.avail_out);

        int data_offset = 0;
        auto fb_width = parent::get_width();
        auto fb_height = parent::get_height();
        for (int y = 0; y < height; y+=64) {
            for (int x = 0; x < width; x+=64) {
                int tile_width = width - x >= 64 ? 64 : width - x;
                int tile_height = height - y >= 64 ? 64 : height - y;
                auto subencoding = data[data_offset];
                data_offset++;
                int bytes_per_cpixel = 3;
                if (subencoding == 0) {
                    for (int x_ = 0; x_ < tile_width; x_++) {
                        for (int y_ = 0; y_ < tile_height; y_++) {
                            auto ptr = std::span{&data[data_offset + (y_*tile_width + x_)*3], 3};
                            frame[(sy+y+y_)*fb_width + (sx+x+x_)] = cpixel_to_pixel(ptr);
                        }
                    }
                    data_offset += tile_width*tile_height*bytes_per_cpixel;
                }
                else if (subencoding == 1) {
                    auto pixel = cpixel_to_pixel(std::span{&data[data_offset], 3});
                    data_offset+=bytes_per_cpixel;
                    for (int x_ = 0; x_ < tile_width; x_++) {
                        for (int y_ = 0; y_ < tile_height; y_++) {
                            frame[(sy+y+y_)*fb_width + (sx+x+x_)] = pixel;
                        }
                    }
                }
                else if (subencoding > 1 && subencoding <= 16) {
                    int palette_size = subencoding;
                    auto palette = std::span{&data[data_offset], palette_size*bytes_per_cpixel};
                    data_offset += palette_size*bytes_per_cpixel;
                    int bits_per_packed_pixels = 1;
                    if (palette_size > 2) bits_per_packed_pixels = 2;
                    if (palette_size > 4) bits_per_packed_pixels = 4;
                    int palette_index_mask = 0b1;
                    if (palette_size > 2) palette_index_mask = 0b11;
                    if (palette_size > 4) palette_index_mask = 0b1111;
                    auto packed_pixels = std::span{&data[data_offset], (tile_width*bits_per_packed_pixels+7)/8*tile_height};
                    data_offset += (tile_width*bits_per_packed_pixels+7)/8*tile_height;

                    for (int y_ = 0; y_ < tile_height; y_++) {
                        auto row_bytes_count = (tile_width*bits_per_packed_pixels+7)/8;
                        auto packed_row_pixels = std::span{&packed_pixels[y_*row_bytes_count], row_bytes_count};
                        for (int x_ = 0; x_ < tile_width; x_++) {
                            auto palette_index = (packed_row_pixels[x_*bits_per_packed_pixels/8] >> ((x_*bits_per_packed_pixels)%8)) & palette_index_mask;
                            frame[(sy+y+y_)*fb_width + (sx+x+x_)] = cpixel_to_pixel(std::span{&palette[palette_index*3], 3});
                        }
                    }
                }
                else if (subencoding == 128) {
                    int run = 0;
                    while (run < tile_width*tile_height) {
                        uint32_t pixel_value = cpixel_to_pixel(std::span{&data[data_offset], 3});
                        data_offset += bytes_per_cpixel;
                        auto ptr = &data[data_offset];
                        int count_255 = 0;
                        while (ptr[count_255] == 255) count_255++;
                        int run_length = 255*count_255 + ptr[count_255] + 1;
                        for (int i = 0; i < run_length; i++) {
                            int y_ = (run+i)/tile_width, x_ = (run+i)%tile_width;
                            frame[(sy+y+y_)*fb_width + (sx+x+x_)] = pixel_value;
                        }

                        run += run_length;
                        data_offset += count_255 + 1;
                    }
                }
                else if (subencoding >= 130 && subencoding <= 255) {
                    int palette_size = subencoding - 128;
                    auto palette = std::span{&data[data_offset], palette_size*bytes_per_cpixel};
                    data_offset += palette_size*bytes_per_cpixel;
                    int run = 0;
                    while (run < tile_width*tile_height) {
                        auto ptr = &data[data_offset];
                        data_offset++;
                        if (ptr[0] < 128) {
                            int palette_index = ptr[0];
                            uint32_t pixel_value = cpixel_to_pixel(std::span{&palette[palette_index*3],3});
                            int run_length = 1;
                            for (int i = 0; i < run_length; i++) {
                                int y_ = (run+i)/tile_width, x_ = (run+i)%tile_width;
                                frame[(sy+y+y_)*fb_width + (sx+x+x_)] = pixel_value;
                            }
                            run++;
                        }
                        else {
                            int palette_index = ptr[0]-128;
                            uint32_t pixel_value = cpixel_to_pixel(std::span{&palette[palette_index*3],3});
                            ptr = &data[data_offset];
                            int count_255 = 0;
                            while (ptr[count_255] == 255) count_255++;
                            int run_length = 255*count_255 + ptr[count_255] + 1;
                            for (int i = 0; i < run_length; i++) {
                                int y_ = (run+i)/tile_width, x_ = (run+i)%tile_width;
                                frame[(sy+y+y_)*fb_width + (sx+x+x_)] = pixel_value;
                            }

                            run += 255*count_255 + ptr[count_255] + 1;
                            data_offset += count_255+1;
                        }
                    }
                }
                else {
                    throw std::runtime_error(std::format("framebuffer update rectangle zrle subencoding not supportted: {}", subencoding));
                }
            }
        }
    }
private:
    z_stream zlib_stream;
}; // class zrle

} //namespace rfb
