/* emugcxbox360 - compiled-code test: renders a Mandelbrot set into the XFB.
 * Exercises compiler output: stack frames, calls, integer + double/single FP.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#define VI16(off) (*(volatile unsigned short *)(0xCC002000 + (off)))
#define VI32(off) (*(volatile unsigned int *)(0xCC002000 + (off)))
#define XFB ((volatile unsigned int *)0xC0500000)

static unsigned int clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

static unsigned int yuyv(float r, float g, float b) {
  float y = 0.257f * r + 0.504f * g + 0.098f * b + 16.0f;
  float u = -0.148f * r - 0.291f * g + 0.439f * b + 128.0f;
  float v = 0.439f * r - 0.368f * g - 0.071f * b + 128.0f;
  unsigned int Y = clamp8((int)y), U = clamp8((int)u), V = clamp8((int)v);
  return (Y << 24) | (U << 16) | (Y << 8) | V;
}

static int iterate(double cx, double cy, int max_iter) {
  double x = 0.0, y = 0.0;
  int i;
  for (i = 0; i < max_iter; i++) {
    double xx = x * x, yy = y * y;
    if (xx + yy > 4.0) break;
    y = 2.0 * x * y + cy;
    x = xx - yy + cx;
  }
  return i;
}

int main(void) {
  VI16(0x00) = (480 << 4) | 6;   /* 480 active lines */
  VI16(0x02) = 0x0005;           /* enable, non-interlaced */
  VI16(0x48) = (40 << 8) | 40;   /* 640 pixels wide */
  VI32(0x1C) = 0x10000000 | (0x00500000 >> 5);

  const int max_iter = 48;
  for (int py = 0; py < 480; py++) {
    double cy = -1.2 + py * (2.4 / 480.0);
    for (int px = 0; px < 640; px += 2) {
      double cx = -2.2 + px * (3.2 / 640.0);
      int n = iterate(cx, cy, max_iter);
      unsigned int c;
      if (n >= max_iter) {
        c = yuyv(0.0f, 0.0f, 0.0f);
      } else {
        float t = (float)n / (float)max_iter;
        c = yuyv(255.0f * t, 255.0f * t * t, 255.0f * (1.0f - t) * 0.6f + 60.0f * t);
      }
      XFB[py * 320 + px / 2] = c;
    }
  }
  for (;;) {}
  return 0;
}
