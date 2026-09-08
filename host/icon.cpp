// Renders the app icon as a transparent RGBA PNG.
//
// Purpose-built rather than cropped from a screenshot -- an icon has to read
// at 32 px, where the game's HUD and instruction text are just mud.
//
// It follows Apple's macOS icon grid rather than filling the tile: on a 1024
// canvas the art occupies a centred 824x824 rounded square (radius ~185), and
// everything outside is transparent. A full-bleed square reads as broken next
// to every other icon in the Dock, which are all that same squircle.
//
//   make icon  ->  host/icon/icon-1024.png  ->  iconutil  ->  AppIcon.icns
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <zlib.h>

static const int N = 1024;

// Apple's macOS icon grid: the shape is inset, not full-bleed.
static const float INSET  = 100.0f;
static const float RADIUS = 185.0f;

// The runner, bolder than the 8x10 in-game sprite: 1 = body, 2 = visor,
// 3 = shoes, 5 = eyes.
static const uint8_t RUNNER[12][10] = {
  {0,0,0,2,2,2,2,0,0,0},
  {0,0,2,2,2,2,2,2,0,0},
  {0,2,2,2,2,2,2,2,2,0},
  {0,2,2,5,2,2,5,2,2,0},
  {0,2,2,2,2,2,2,2,2,0},
  {0,0,2,2,2,2,2,2,0,0},
  {0,1,1,1,1,1,1,1,1,0},
  {1,1,1,1,1,1,1,1,1,1},
  {1,1,0,1,1,1,1,0,1,1},
  {0,0,0,1,1,1,1,0,0,0},
  {0,0,3,3,3,0,3,3,3,0},
  {0,3,3,3,0,0,0,3,3,3},
};

struct RGB { uint8_t r, g, b; };
static RGB img[N][N];

static RGB lerp(RGB a, RGB b, float t) {
  return { (uint8_t)(a.r + (b.r - a.r) * t),
           (uint8_t)(a.g + (b.g - a.g) * t),
           (uint8_t)(a.b + (b.b - a.b) * t) };
}
static void px(int x, int y, RGB c) {
  if (x >= 0 && y >= 0 && x < N && y < N) img[y][x] = c;
}
static void box(int x, int y, int w, int h, RGB c) {
  for (int j = y; j < y + h; j++)
    for (int i = x; i < x + w; i++) px(i, j, c);
}

// Signed distance to a rounded rectangle: negative inside. Used for a
// one-pixel antialiased edge, so the icon does not look jagged at 512.
static float roundedRectSDF(float x, float y) {
  const float cx = N * 0.5f, cy = N * 0.5f;
  const float bx = (N - 2 * INSET) * 0.5f - RADIUS;
  const float by = bx;
  const float qx = fabsf(x - cx) - bx;
  const float qy = fabsf(y - cy) - by;
  const float ax = fmaxf(qx, 0.0f), ay = fmaxf(qy, 0.0f);
  return sqrtf(ax * ax + ay * ay) + fminf(fmaxf(qx, qy), 0.0f) - RADIUS;
}

// ---- minimal RGBA PNG (zlib ships with macOS) ------------------------------
static void be32(std::vector<uint8_t> &v, uint32_t x) {
  v.push_back(x >> 24); v.push_back(x >> 16); v.push_back(x >> 8); v.push_back(x);
}
static void chunk(FILE *f, const char *type, const std::vector<uint8_t> &data) {
  std::vector<uint8_t> hdr;
  be32(hdr, (uint32_t)data.size());
  fwrite(hdr.data(), 1, hdr.size(), f);

  std::vector<uint8_t> body(type, type + 4);
  body.insert(body.end(), data.begin(), data.end());
  fwrite(body.data(), 1, body.size(), f);

  std::vector<uint8_t> crc;
  be32(crc, (uint32_t)crc32(crc32(0, Z_NULL, 0), body.data(), (uInt)body.size()));
  fwrite(crc.data(), 1, crc.size(), f);
}

static bool writePNG(const char *path, const std::vector<uint8_t> &rgba) {
  FILE *f = fopen(path, "wb");
  if (!f) return false;

  static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
  fwrite(sig, 1, 8, f);

  std::vector<uint8_t> ihdr;
  be32(ihdr, N); be32(ihdr, N);
  ihdr.push_back(8);            // bit depth
  ihdr.push_back(6);            // RGBA
  ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
  chunk(f, "IHDR", ihdr);

  // Raw scanlines, each with filter byte 0.
  std::vector<uint8_t> raw;
  raw.reserve((size_t)N * (N * 4 + 1));
  for (int y = 0; y < N; y++) {
    raw.push_back(0);
    raw.insert(raw.end(), rgba.begin() + (size_t)y * N * 4,
                          rgba.begin() + (size_t)(y + 1) * N * 4);
  }

  uLongf zlen = compressBound((uLong)raw.size());
  std::vector<uint8_t> z(zlen);
  if (compress2(z.data(), &zlen, raw.data(), (uLong)raw.size(), 9) != Z_OK) {
    fclose(f); return false;
  }
  z.resize(zlen);
  chunk(f, "IDAT", z);
  chunk(f, "IEND", {});
  fclose(f);
  return true;
}

int main() {
  const RGB TOP   = {  18,   0,  46 };
  const RGB MID   = { 104,  12, 124 };
  const RGB HOT   = { 232,  44, 118 };
  const RGB GLOW  = { 255, 138,  32 };
  const RGB ROCK  = {  40,   8,  62 };
  const RGB CYAN  = {  32, 240, 255 };
  const RGB MAG   = { 245,  40, 200 };
  const RGB ORANG = { 255, 150,  30 };
  const RGB WHITE = { 255, 255, 255 };

  const int HORIZON = (int)(N * 0.70f);

  for (int y = 0; y < HORIZON; y++) {
    const float t = (float)y / HORIZON;
    RGB c = (t < 0.45f) ? lerp(TOP, MID, t / 0.45f)
          : (t < 0.82f) ? lerp(MID, HOT, (t - 0.45f) / 0.37f)
                        : lerp(HOT, GLOW, (t - 0.82f) / 0.18f);
    for (int x = 0; x < N; x++) img[y][x] = c;
  }

  // Sun, slits widening towards the horizon
  const int cx = N / 2, cy = (int)(N * 0.38f), r = (int)(N * 0.23f);
  for (int dy = -r; dy <= r; dy++) {
    const int y = cy + dy;
    if (y < 0 || y >= HORIZON) continue;
    if (dy > -r / 3) {
      const int period = N / 22, cut = 1 + (dy + r / 3) / (N / 9);
      if (((dy + r / 3) % period) < cut * (N / 200)) continue;
    }
    const int half = (int)sqrtf((float)(r * r - dy * dy));
    const float t = (float)(dy + r) / (2 * r);
    const RGB c = lerp({ 255, 236, 96 }, { 255, 74, 118 }, t);
    for (int x = cx - half; x <= cx + half; x++) px(x, y, c);
  }

  box(0, HORIZON, N, N - HORIZON, ROCK);
  box(0, HORIZON, N, N / 46, CYAN);
  for (int k = 1; k <= 3; k++)
    box(0, HORIZON + k * k * (N / 60) + N / 22, N, N / 220, { 60, 30, 120 });

  // Runner, feet on the ground line
  const int s = N / 28;
  const int rw = 10 * s, rh = 12 * s;
  const int rx = cx - rw / 2, ry = HORIZON - rh + s;
  const RGB EDGE = { 12, 0, 24 };
  for (int j = 0; j < 12; j++)
    for (int i = 0; i < 10; i++)
      if (RUNNER[j][i])
        for (int dy = -1; dy <= 1; dy++)
          for (int dx = -1; dx <= 1; dx++)
            box(rx + (i + dx) * s, ry + (j + dy) * s, s, s, EDGE);
  for (int j = 0; j < 12; j++)
    for (int i = 0; i < 10; i++) {
      const uint8_t p = RUNNER[j][i];
      if (!p) continue;
      const RGB c = (p == 1) ? MAG : (p == 2) ? CYAN : (p == 3) ? ORANG : WHITE;
      box(rx + i * s, ry + j * s, s, s, c);
    }

  // Mask to the icon-grid squircle, antialiased, transparent outside.
  std::vector<uint8_t> rgba((size_t)N * N * 4);
  for (int y = 0; y < N; y++)
    for (int x = 0; x < N; x++) {
      const float d = roundedRectSDF(x + 0.5f, y + 0.5f);
      float a = 0.5f - d;
      a = a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a);
      const size_t o = ((size_t)y * N + x) * 4;
      rgba[o + 0] = img[y][x].r;
      rgba[o + 1] = img[y][x].g;
      rgba[o + 2] = img[y][x].b;
      rgba[o + 3] = (uint8_t)(a * 255.0f + 0.5f);
    }

  if (!writePNG("host/icon/icon-1024.png", rgba)) {
    fprintf(stderr, "cannot write host/icon/icon-1024.png\n");
    return 1;
  }
  printf("wrote host/icon/icon-1024.png\n");
  return 0;
}
