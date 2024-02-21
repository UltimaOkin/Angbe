/*
    Big ComBoy
    Copyright (C) 2023-2024 UltimaOmega474

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "PPU.hpp"
#include "Bus.hpp"
#include <algorithm>
#include <span>

namespace GB
{
    PPU::PPU(MainBus &bus) : bus(bus) {}

    void PPU::reset(bool hard_reset)
    {
        if (hard_reset)
        {
            status = 0;
            lcd_control = 0;
            background_palette = 0;
            object_palette_0 = 0;
            object_palette_1 = 0;
            screen_scroll_y = 0;
            screen_scroll_x = 0;
            window_y = 0;
            window_x = 0;
            line_y_compare = 0;
            vram.fill(0);
            oam.fill(0);
            framebuffer.fill(0);
            framebuffer_complete.fill(0);
            bg_color_table.fill(0);
            objects_on_scanline.fill({});
            mode = PPUState::HBlank;
        }
        window_draw_flag = false;
        num_obj_on_scanline = 0;
        cycles = 0;
        line_y = 0;

        window_line_y = 0;
    }

    void PPU::set_post_boot_state()
    {
        window_draw_flag = true;
        previously_disabled = false;
        cycles = 420;
        status = 1;
        lcd_control = 0x91;
    }

    void PPU::step(uint32_t accumulated_cycles)
    {
        if (!(lcd_control & LCDControlFlags::DisplayEnable))
        {
            mode = HBlank;
            set_stat(ModeFlag, false);
            status |= mode & 0x03;
            previously_disabled = true;
            return;
        }

        if (previously_disabled)
        {
            reset(false);
            previously_disabled = false;
        }

        cycles += accumulated_cycles;
        bool allow_interrupt = stat_any() ? false : true;

        switch (mode)
        {

        case PPUState::HBlank:
        {
            if (cycles >= 204)
            {
                cycles -= 204;
                ++line_y;

                if (line_y == 144)
                {
                    mode = PPUState::VBlank;

                    bus.request_interrupt(INT_VBLANK_BIT);
                    if (check_stat(EnableVBlankInt) && allow_interrupt)
                        bus.request_interrupt(INT_LCD_STAT_BIT);
                }
                else
                {

                    mode = PPUState::OAMSearch;

                    if (check_stat(EnableOAMInt) && allow_interrupt)
                        bus.request_interrupt(INT_LCD_STAT_BIT);
                }
            }
            break;
        }

        case PPUState::VBlank:
        {
            if (cycles >= 456)
            {
                ++line_y;
                cycles -= 456;

                if (line_y > 153)
                {
                    framebuffer_complete = framebuffer;
                    mode = PPUState::OAMSearch;
                    if (check_stat(EnableOAMInt) && allow_interrupt)
                        bus.request_interrupt(INT_LCD_STAT_BIT);

                    line_y = 0;
                    window_line_y = 0;
                    window_draw_flag = false;
                }
            }
            break;
        }

        case PPUState::OAMSearch:
        {
            if (cycles >= 80)
            {
                scan_oam();
                cycles -= 80;
                mode = PPUState::DrawScanline;
                break;
            }

            break;
        }

        case PPUState::DrawScanline:
        {
            if (cycles >= 172)
            {
                cycles -= 172;
                mode = PPUState::HBlank;

                if (check_stat(EnableHBlankInt) && allow_interrupt)
                    bus.request_interrupt(INT_LCD_STAT_BIT);

                render_scanline();
            }
            break;
        }
        }

        check_ly_lyc(allow_interrupt);

        if (window_y == line_y)
            window_draw_flag = true;

        set_stat(ModeFlag, false);
        status |= mode & 0x03;
    }

    void PPU::write_vram(uint16_t address, uint8_t value) { vram[address] = value; }

    void PPU::write_oam(uint16_t address, uint8_t value) { oam[address] = value; }

    void PPU::instant_dma(uint8_t address)
    {
        uint16_t addr = address << 8;
        for (auto i = 0; i < 160; ++i)
        {
            oam[i] = bus.read((addr) + i);
        }
    }

    uint8_t PPU::read_vram(uint16_t address) const { return vram[address]; }

    uint8_t PPU::read_oam(uint16_t address) const { return oam[address]; }

    bool PPU::check_stat(uint8_t flags) const { return status & flags; }

    void PPU::set_stat(uint8_t flags, bool value)
    {
        if (value)
        {
            status |= flags;
        }
        else
        {
            status &= ~flags;
        }
    }

    bool PPU::stat_any() const
    {
        if (check_stat(EnableLYCandLYInt))
        {
            if (check_stat(LYCandLYCompareType))
                return true;
        }

        if (check_stat(EnableOAMInt))
        {
            if (mode == 2)
                return true;
        }

        if (check_stat(EnableVBlankInt))
        {
            if (mode == 1)
                return true;
        }

        if (check_stat(EnableHBlankInt))
        {
            if (mode == 0)
                return true;
        }

        return false;
    }

    void PPU::render_scanline()
    {
        render_bg_layer();
        if (render_flags & DisplayRenderFlags::Window)
            render_window_layer();
        if (render_flags & DisplayRenderFlags::Objects)
            render_sprite_layer();
    }

    void PPU::render_bg_layer()
    {
        uint16_t tile_map_address = (lcd_control & LCDControlFlags::BGTileMap) ? 0x9c00 : 0x9800;
        uint16_t tile_data_address =
            (lcd_control & LCDControlFlags::BGWindowTileData) ? 0x8000 : 0x8800;

        for (uint8_t i = 0; i < 160; ++i)
        {
            uint8_t x_offset = screen_scroll_x + i;
            uint8_t y_offset = screen_scroll_y + line_y;
            uint8_t tile_id = vram[(tile_map_address & 0x1FFF) + ((x_offset / 8) & 31) +
                                   (((y_offset / 8) & 31) * 32)];
            uint16_t tile_index = 0;

            if (tile_data_address == 0x8800)
                tile_index = ((tile_data_address + 0x800) & 0x1FFF) + (((int8_t)tile_id) * 16) +
                             ((y_offset & 7) * 2);
            else
                tile_index = (tile_data_address & 0x1FFF) + (tile_id * 16) + ((y_offset & 7) * 2);

            uint8_t low_byte = vram[tile_index];
            uint8_t high_byte = vram[tile_index + 1];
            uint8_t bit = 7 - (x_offset & 7);
            uint8_t low_bit = (low_byte >> bit) & 0x01;
            uint8_t high_bit = (high_byte >> bit) & 0x01;
            uint8_t pixel = (high_bit << 1) | low_bit;

            size_t framebuffer_line_x = i;
            size_t framebuffer_line_y = line_y * LCD_WIDTH;

            std::array<uint8_t, 4> color =
                color_table[(background_palette >> (int)(2 * pixel)) & 3];

            if (!(lcd_control & LCDControlFlags::BGEnable) ||
                !(render_flags & DisplayRenderFlags::Background))
            {
                color = color_table[(background_palette) & 3];
                bg_color_table[framebuffer_line_y + i] = 0;
            }
            else
            {
                bg_color_table[framebuffer_line_y + i] = pixel;
            }

            auto fb_pixel = std::span<uint8_t>{
                &framebuffer[(framebuffer_line_y + framebuffer_line_x) * COLOR_DEPTH], 4};

            std::copy(color.begin(), color.end(), fb_pixel.begin());
        }
    }

    void PPU::render_window_layer()
    {
        uint16_t tile_map_address =
            (lcd_control & LCDControlFlags::WindowTileMap) ? 0x9c00 : 0x9800;
        uint16_t tile_data_address =
            (lcd_control & LCDControlFlags::BGWindowTileData) ? 0x8000 : 0x8800;
        uint8_t advance_by = 0;
        if ((lcd_control & LCDControlFlags::WindowEnable) && window_draw_flag &&
            (render_flags & DisplayRenderFlags::Window))
        {
            for (uint8_t i = 0; i < LCD_WIDTH; ++i)
            {
                if (line_y >= window_y && i >= (window_x - 7))
                {
                    uint8_t x_offset = (i - (window_x - 7));
                    uint8_t y_offset = window_line_y;
                    uint8_t tile_id = vram[(tile_map_address & 0x1FFF) + ((x_offset / 8) & 31) +
                                           (((y_offset / 8) & 31) * 32)];
                    uint16_t tile_index = 0;

                    if (tile_data_address == 0x8800)
                        tile_index = ((tile_data_address + 0x800) & 0x1FFF) +
                                     (((int8_t)tile_id) * 16) + ((y_offset & 7) * 2);
                    else
                        tile_index =
                            (tile_data_address & 0x1FFF) + (tile_id * 16) + ((y_offset & 7) * 2);

                    uint8_t low_byte = vram[tile_index];
                    uint8_t high_byte = vram[tile_index + 1];
                    uint8_t bit = 7 - (x_offset & 7);
                    uint8_t low_bit = (low_byte >> bit) & 0x01;
                    uint8_t high_bit = (high_byte >> bit) & 0x01;
                    uint8_t pixel = (high_bit << 1) | low_bit;

                    size_t framebuffer_line_x = i;
                    size_t framebuffer_line_y = line_y * LCD_WIDTH;
                    advance_by = 1;

                    bg_color_table[framebuffer_line_y + framebuffer_line_x] = pixel;

                    std::array<uint8_t, 4> color =
                        color_table[(background_palette >> (int)(2 * pixel)) & 3];
                    auto fb_pixel = std::span<uint8_t>{
                        &framebuffer[(framebuffer_line_y + framebuffer_line_x) * COLOR_DEPTH], 4};

                    std::copy(color.begin(), color.end(), fb_pixel.begin());
                }
            }
        }

        window_line_y += advance_by;
    }

    void PPU::scan_oam()
    {
        uint8_t height = (lcd_control & LCDControlFlags::SpriteSize) ? 16 : 8;

        num_obj_on_scanline = 0;
        objects_on_scanline.fill({});

        for (auto i = 0, total = 0; i < 40; ++i)
        {
            if (total == 10)
                break;

            const Object *sprite = reinterpret_cast<const Object *>((&oam[i * 4]));
            int16_t corrected_y_position = (int16_t)sprite->y - 16;

            if (corrected_y_position <= line_y && (corrected_y_position + height) > line_y)
            {
                objects_on_scanline[total] = *sprite;
                total++;
                num_obj_on_scanline++;
            }
        }

        std::stable_sort(
            objects_on_scanline.begin(), objects_on_scanline.begin() + num_obj_on_scanline,
            [=](const Object &left, const Object &right) { return (left.x) < (right.x); });
    }

    void PPU::render_sprite_layer()
    {
        uint8_t height = (lcd_control & LCDControlFlags::SpriteSize) ? 16 : 8;

        if ((lcd_control & LCDControlFlags::SpriteEnable) &&
            (render_flags & DisplayRenderFlags::Objects))
        {
            for (auto i = 0; i < num_obj_on_scanline; ++i)
            {
                auto &object = objects_on_scanline[(num_obj_on_scanline - 1) - i];

                uint8_t palette = (object.attributes & ObjectAttributeFlags::Palette)
                                      ? object_palette_1
                                      : object_palette_0;
                uint16_t tile_index = 0;

                if (height == 16)
                    object.tile &= ~1;

                int16_t obj_y = (int16_t)object.y - 16;

                if (object.attributes & ObjectAttributeFlags::FlipY)
                    tile_index = (0x8000 & 0x1FFF) + (object.tile * 16) +
                                 ((height - (line_y - obj_y) - 1) * 2);
                else
                    tile_index =
                        (0x8000 & 0x1FFF) + (object.tile * 16) + ((line_y - obj_y) % height * 2);

                int16_t adjusted_x = (int16_t)object.x - 8;
                size_t framebuffer_line_y = line_y * LCD_WIDTH;

                for (auto x = 0; x < 8; ++x)
                {
                    size_t framebuffer_line_x = adjusted_x + x;

                    if (framebuffer_line_x >= 0 && framebuffer_line_x < 160)
                    {
                        uint8_t low_byte = vram[tile_index];
                        uint8_t high_byte = vram[tile_index + 1];
                        uint8_t bit = 7 - (x & 7);

                        if (object.attributes & ObjectAttributeFlags::FlipX)
                            bit = x & 7;

                        uint8_t low_bit = (low_byte >> bit) & 0x01;
                        uint8_t high_bit = (high_byte >> bit) & 0x01;
                        uint8_t pixel = (high_bit << 1) | low_bit;

                        if (pixel == 0)
                            continue;

                        if (!(object.attributes & ObjectAttributeFlags::Priority))
                        {
                            std::array<uint8_t, 4> color =
                                color_table[(palette >> (int)(2 * pixel)) & 3];
                            auto fb_pixel = std::span<uint8_t>{
                                &framebuffer[(framebuffer_line_y + framebuffer_line_x) *
                                             COLOR_DEPTH],
                                4};

                            std::copy(color.begin(), color.end(), fb_pixel.begin());
                        }
                        else
                        {
                            if (bg_color_table[framebuffer_line_y + framebuffer_line_x] == 0)
                            {

                                std::array<uint8_t, 4> color =
                                    color_table[(palette >> (int)(2 * pixel)) & 3];
                                auto fb_pixel = std::span<uint8_t>{
                                    &framebuffer[(framebuffer_line_y + framebuffer_line_x) *
                                                 COLOR_DEPTH],
                                    4};

                                std::copy(color.begin(), color.end(), fb_pixel.begin());
                            }
                        }
                    }
                }
            }
        }
    }

    void PPU::check_ly_lyc(bool allow_interrupts)
    {
        set_stat(LYCandLYCompareType, false);
        if (line_y == line_y_compare)
        {
            set_stat(LYCandLYCompareType, true);
            if (check_stat(EnableLYCandLYInt) && allow_interrupts)
            {
                bus.request_interrupt(INT_LCD_STAT_BIT);
            }
        }
    }
}