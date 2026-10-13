// SPDX-License-Identifier: GPL-3.0-or-later
// Renders a chat message with colour emoji at the T-Deck's text sizes, before and
// after src/ui-touch/EmojiFit.h, and writes a 24-bit BMP for review. The message is
// an IceBot report: emoji at the start, middle and end of lines, beside digits.
//
//   render <out.bmp>
#include "lvgl.h"
#include "../../src/ui-touch/EmojiFit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const int W = 800, H = 190;
static uint8_t* s_rgb = nullptr;

static void flush(lv_disp_drv_t* drv, const lv_area_t* a, lv_color_t* px) {
  for (int y = a->y1; y <= a->y2; ++y)
    for (int x = a->x1; x <= a->x2; ++x) {
      const uint32_t c = lv_color_to32(*px++);
      uint8_t* o = s_rgb + ((size_t)y * W + x) * 3;
      o[0] = (c >> 16) & 0xFF; o[1] = (c >> 8) & 0xFF; o[2] = c & 0xFF;
    }
  lv_disp_flush_ready(drv);
}

static bool writeBmp(const char* path) {
  FILE* f = fopen(path, "wb");
  if (!f) return false;
  const uint32_t row = (uint32_t)((W * 3 + 3) & ~3), img = row * (uint32_t)H;
  uint8_t h[54] = {'B', 'M'};
  auto p32 = [&](int o, uint32_t v) { h[o] = v; h[o + 1] = v >> 8; h[o + 2] = v >> 16; h[o + 3] = v >> 24; };
  p32(2, 54 + img); p32(10, 54); p32(14, 40); p32(18, (uint32_t)W); p32(22, (uint32_t)H);
  h[26] = 1; h[28] = 24; p32(34, img);
  fwrite(h, 1, sizeof h, f);
  uint8_t pad[3] = {0, 0, 0};
  for (int y = H - 1; y >= 0; --y) {
    for (int x = 0; x < W; ++x) {
      const uint8_t* c = s_rgb + ((size_t)y * W + x) * 3;
      const uint8_t bgr[3] = {c[2], c[1], c[0]};
      fwrite(bgr, 1, 3, f);
    }
    fwrite(pad, 1, row - (uint32_t)W * 3, f);
  }
  fclose(f);
  return true;
}

// The firmware's old path callback: the baked 16 px image, unfitted.
static bool oldPathCb(const lv_font_t*, void* img_src, uint16_t, uint32_t unicode, uint32_t) {
  const lv_img_dsc_t* d = emojiGlyphLookup(unicode);
  if (!d) return false;
  memcpy(img_src, d, sizeof(lv_img_dsc_t));
  lv_img_cache_invalidate_src(img_src);
  return true;
}

static const char* kMsg =
    "IceBot \xF0\x9F\xA4\x96\xF0\x9F\x9A\xAB\xF0\x9F\xA7\x8A\n"          // robot, no entry, ice
    "\xF0\x9F\x94\xB4 CRITICAL\n"                                          // red circle
    "10-12 10:30\n"
    "\xF0\x9F\xA7\x8A @Tacoma S Hosmer St\n"                               // ice
    "6-\xF0\x9F\xA5\xBE  3-\xF0\x9F\x9A\x98  \xF0\x9F\x9F\xA2\xF0\x9F\x9F\xA1\xF0\x9F\x94\xB5\n"  // boot, car, circles
    "\xF0\x9F\x93\x8D 47.1745,-122.4639 \xF0\x9F\x9A\xA8";              // pin, siren

static void bubble(lv_obj_t* scr, int x, int y, const lv_font_t* font, const char* title) {
  lv_obj_t* t = lv_label_create(scr);
  lv_label_set_text(t, title);
  lv_obj_set_style_text_color(t, lv_color_hex(0x8A97A6), 0);
  lv_obj_set_style_text_font(t, &lv_font_montserrat_12, 0);
  lv_obj_set_pos(t, x, y);
  lv_obj_t* b = lv_obj_create(scr);
  lv_obj_remove_style_all(b);
  lv_obj_set_style_bg_color(b, lv_color_hex(0x2C4A6E), 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(b, 9, 0);
  lv_obj_set_style_pad_hor(b, 8, 0);
  lv_obj_set_style_pad_ver(b, 5, 0);
  lv_obj_set_size(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_pos(b, x, y + 18);
  lv_obj_t* l = lv_label_create(b);
  lv_label_set_text(l, kMsg);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(0xE6EBED), 0);
}

int main(int argc, char** argv) {
  if (argc < 2) { fprintf(stderr, "usage: render <out.bmp>\n"); return 2; }
  lv_init();
  s_rgb = (uint8_t*)calloc((size_t)W * H, 3);
  static lv_disp_draw_buf_t db;
  static lv_color_t buf[W * H];
  lv_disp_draw_buf_init(&db, buf, nullptr, W * H);
  static lv_disp_drv_t drv;
  lv_disp_drv_init(&drv);
  drv.hor_res = W; drv.ver_res = H; drv.flush_cb = flush; drv.draw_buf = &db; drv.full_refresh = 1;
  lv_disp_drv_register(&drv);
  lv_obj_t* scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x111418), 0);

  // Before: the 12 and 14 px text with the plain imgfont behind them.
  static lv_font_t old12 = lv_font_montserrat_12, old14 = lv_font_montserrat_14;
  old12.fallback = lv_imgfont_create(16, oldPathCb);
  old14.fallback = lv_imgfont_create(16, oldPathCb);
  // After: the same, fitted.
  static lv_font_t new12 = lv_font_montserrat_12, new14 = lv_font_montserrat_14;
  lv_font_t* e12 = lv_imgfont_create(16, emojifit::pathCb);
  lv_font_t* e14 = lv_imgfont_create(16, emojifit::pathCb);
  emojifit::attach(e12, &new12);
  emojifit::attach(e14, &new14);
  new12.fallback = e12;
  new14.fallback = e14;

  bubble(scr, 8, 6, &old12, "12 px before");
  bubble(scr, 194, 6, &new12, "12 px after");
  bubble(scr, 380, 6, &old14, "14 px before");
  bubble(scr, 590, 6, &new14, "14 px after");
  for (int t = 0; t < 200; t += 10) { lv_tick_inc(10); lv_timer_handler(); }
  lv_obj_invalidate(scr);
  lv_refr_now(nullptr);
  return writeBmp(argv[1]) ? 0 : 1;
}
