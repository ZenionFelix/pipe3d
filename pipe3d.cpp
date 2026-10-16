// Meja kerja 3D untuk Minecraft PE 0.15.10 (armeabi-v7a)
// Versi 6: meja kerja (kotak) + tungku ke lapisan transparan + daun rimbun 8 bidang (gaya video) + log kecil.
#include <android/log.h>
#include <link.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define TAG "pipe3d"
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

// Alamat di libminecraftpe.so 0.15.10 (dari dump dan disassembly)
// BlockTessellator::tessellateBlockInWorld(Block&, BlockPos const&, int)
static const uintptr_t OFF_TESS_BLOCK = 0x993E20;
// Awal fungsi harus persis ini, kalau tidak hook dibatalkan
static const uint8_t SIG_TESS_BLOCK[8] = {0x2d, 0xe9, 0xf0, 0x4b, 0x03, 0xaf, 0x90, 0xb0};

static const int BOUNDS_OFF = 0x2d4;   // AABB (6 float + 1 byte) di dalam BlockTessellator
static const int BLOCK_ID_OFF = 4;     // id blok (1 byte) di dalam Block
static const int ID_CRAFTING_TABLE = 58;
static const int ID_FURNACE = 61;
static const int ID_FURNACE_LIT = 62;
static const int LAYER_OFF = 0x14;     // lapisan gambar blok (int) di dalam Block
static const int FLAG_OFF = 0x10;      // penanda pendukung alpha test (1 byte)
static const int LAYER_ALPHATEST = 4;
static const int ID_LEAVES = 18;       // daun oak, spruce, birch, jungle
static const int ID_LEAVES2 = 161;     // daun acacia, dark oak
static const int TESS_OFF = 0x2d0;     // penunjuk Tessellator di dalam BlockTessellator
// Fungsi game (alamat 0.15.10, bit terakhir 1 = kode Thumb)
static const uintptr_t OFF_VERTEX_UV = 0xA23C19;   // Tessellator::vertexUV(x,y,z,u,v)
static const uintptr_t OFF_QUAD = 0xA24235;        // Tessellator::quad(bool)
static const uintptr_t OFF_GET_TEXTURE = 0x995799; // BlockTessellator::_getTexture(Block&, face, data)
static const uintptr_t OFF_COLOR = 0xA23B89;       // Tessellator::color(int 0xRRGGBB)
static const int VT_GET_COLOR = 0x14c;             // Block vtable: getColor(BlockSource&, BlockPos const&)

// Catat nilai lapisan tiap id blok sekali saja, untuk bahan pelajaran
static bool seen_id[256];
static void log_block(int id, void* block) {
  if (id < 0 || id > 255 || seen_id[id]) return;
  seen_id[id] = true;
  FILE* f = fopen("/sdcard/games/com.mojang/pipe3d_log.txt", "a");
  if (!f) return;
  fprintf(f, "id=%d layer=%d flag=%d\n", id,
          *(int*)((uint8_t*)block + LAYER_OFF), *((uint8_t*)block + FLAG_OFF));
  fclose(f);
}

// Kotak meja kerja dalam 1/16 blok: x0,y0,z0,x1,y1,z1
static const int8_t BOXES[][6] = {
  {0,0,0,16,1,16},{0,15,0,16,16,16},{0,1,0,1,15,1},{15,1,0,16,15,1},
  {0,1,15,1,15,16},{15,1,15,16,15,16},{1,0,1,15,16,15},{6,14,0,10,15,1},
  {7,1,0,9,14,1},{6,14,15,10,15,16},{7,1,15,9,14,16},{0,14,6,1,15,10},
  {0,1,7,1,14,9},{15,14,6,16,15,10},{15,1,7,16,14,9},{12,7,0,13,11,1},
  {2,10,0,3,13,1},{4,10,0,5,13,1},{3,10,0,4,11,1},{3,12,0,4,13,1},
  {2,8,0,5,10,1},{2,5,0,4,8,1},{2,3,0,3,5,1},{12,6,0,14,7,1},
  {11,5,0,12,8,1},{3,7,15,4,11,16},{11,10,15,12,13,16},{13,10,15,14,13,16},
  {12,10,15,13,11,16},{12,12,15,13,13,16},{11,8,15,14,10,16},{12,5,15,14,8,16},
  {13,3,15,14,5,16},{2,6,15,4,7,16},{4,5,15,5,8,16},{0,9,3,1,11,4},
  {0,9,5,1,11,6},{0,6,3,1,8,4},{0,6,5,1,8,6},{0,8,4,1,9,5},
  {15,9,10,16,11,11},{15,9,12,16,11,13},{15,6,10,16,8,11},{15,6,12,16,8,13},
  {15,8,11,16,9,12}
};
static const int NUM_BOXES = sizeof(BOXES) / sizeof(BOXES[0]);

typedef int (*TessBlockFn)(void* self, void* block, void* pos, int data);
typedef void (*VertexUVFn)(void* tess, float x, float y, float z, float u, float v);
typedef void (*QuadFn)(void* tess, bool flip);
typedef const float* (*GetTexFn)(void* self, void* block, signed char face, int data);
typedef void (*ColorFn)(void* tess, int rgb);
typedef int (*GetColorFn)(void* block, void* source, void* pos);
static ColorFn fn_color = nullptr;
static VertexUVFn fn_vertex_uv = nullptr;
static QuadFn fn_quad = nullptr;
static GetTexFn fn_get_tex = nullptr;
static bool leaf_logged = false;

// Satu bidang dari 4 titik, digambar dua sisi
static void draw_quad(void* tess, float bx, float by, float bz,
                      const float p[4][3], const float uv[4][2]) {
  for (int i = 0; i < 4; i++)
    fn_vertex_uv(tess, bx + p[i][0], by + p[i][1], bz + p[i][2], uv[i][0], uv[i][1]);
  fn_quad(tess, false);
  fn_quad(tess, true);
}
static TessBlockFn orig_tess_block = nullptr;


static void set_shaded_color(void* tess, int c, float k) {
  int r = (int)(((c >> 16) & 255) * k);
  int g = (int)(((c >> 8) & 255) * k);
  int b = (int)((c & 255) * k);
  fn_color(tess, (r << 16) | (g << 8) | b);
}

// Daun gaya video: 4 bidang miring 45 derajat (dua miring ke arah Z, dua ke arah X), tanpa kubus.
// Mengembalikan true kalau berhasil digambar.
static bool draw_leaf_planes(void* self, void* block, void* pos, int data) {
  void* tess = *(void**)((uint8_t*)self + TESS_OFF);
  if (!tess || !fn_vertex_uv || !fn_quad || !fn_get_tex || !fn_color) return false;
  const float* t = fn_get_tex(self, block, 2, data);
  if (!leaf_logged) {
    leaf_logged = true;
    FILE* f = fopen("/sdcard/games/com.mojang/pipe3d_log.txt", "a");
    if (f) {
      if (t) fprintf(f, "leaf tess=%p uv=%f %f %f %f\n", tess, t[0], t[1], t[2], t[3]);
      else fprintf(f, "leaf tess=%p uv=null\n", tess);
      fclose(f);
    }
  }
  if (!(t && t[0] >= 0.0f && t[1] >= 0.0f && t[2] <= 1.001f && t[3] <= 1.001f &&
        t[2] > t[0] && t[3] > t[1])) return false;
  float u0 = t[0], v0 = t[1], u1 = t[2], v1 = t[3];

  int tint = 0x00FFFFFF;
  void** vt = *(void***)block;
  GetColorFn get_color = (GetColorFn)vt[VT_GET_COLOR / 4];
  void* source = *(void**)((uint8_t*)self + 4);
  if (source) tint = get_color(block, source, pos);

  int* bp = (int*)pos;
  float bx = (float)bp[0], by = (float)bp[1], bz = (float)bp[2];
  const float uv[4][2] = {{u0, v1}, {u1, v1}, {u1, v0}, {u0, v0}};
  const float p1[4][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 1}, {0, 1, 1}};
  const float p2[4][3] = {{0, 0, 1}, {1, 0, 1}, {1, 1, 0}, {0, 1, 0}};
  const float p3[4][3] = {{0, 0, 0}, {0, 0, 1}, {1, 1, 1}, {1, 1, 0}};
  const float p4[4][3] = {{1, 0, 0}, {1, 0, 1}, {0, 1, 1}, {0, 1, 0}};
  // bidang tegak diagonal (dua) dan bidang datar (dua) untuk mengisi celah dari segala arah
  const float p5[4][3] = {{0, 0, 0}, {1, 0, 1}, {1, 1, 1}, {0, 1, 0}};
  const float p6[4][3] = {{1, 0, 0}, {0, 0, 1}, {0, 1, 1}, {1, 1, 0}};
  const float p7[4][3] = {{0, 0.30f, 1}, {1, 0.30f, 1}, {1, 0.30f, 0}, {0, 0.30f, 0}};
  const float p8[4][3] = {{0, 0.70f, 1}, {1, 0.70f, 1}, {1, 0.70f, 0}, {0, 0.70f, 0}};
  // terang-gelap dibuat lebih dalam supaya rumpun terlihat tebal dan berkedalaman
  set_shaded_color(tess, tint, 0.95f); draw_quad(tess, bx, by, bz, p1, uv);
  set_shaded_color(tess, tint, 0.70f); draw_quad(tess, bx, by, bz, p2, uv);
  set_shaded_color(tess, tint, 0.85f); draw_quad(tess, bx, by, bz, p3, uv);
  set_shaded_color(tess, tint, 0.62f); draw_quad(tess, bx, by, bz, p4, uv);
  set_shaded_color(tess, tint, 0.78f); draw_quad(tess, bx, by, bz, p5, uv);
  set_shaded_color(tess, tint, 0.66f); draw_quad(tess, bx, by, bz, p6, uv);
  set_shaded_color(tess, tint, 0.58f); draw_quad(tess, bx, by, bz, p7, uv);
  set_shaded_color(tess, tint, 0.90f); draw_quad(tess, bx, by, bz, p8, uv);
  return true;
}

static int hook_tess_block(void* self, void* block, void* pos, int data) {
  int id = *((uint8_t*)block + BLOCK_ID_OFF);
  log_block(id, block);
  if (id == ID_FURNACE || id == ID_FURNACE_LIT) {
    *(int*)((uint8_t*)block + LAYER_OFF) = LAYER_ALPHATEST;
    *((uint8_t*)block + FLAG_OFF) = 1;
  }
  if (id == ID_LEAVES || id == ID_LEAVES2) {
    if (draw_leaf_planes(self, block, pos, data)) return 1;
    return orig_tess_block(self, block, pos, data);   // cadangan kalau gagal
  }
  if (id != ID_CRAFTING_TABLE) {
    return orig_tess_block(self, block, pos, data);
  }
  uint8_t* b = (uint8_t*)self + BOUNDS_OFF;
  uint8_t saved[28];
  memcpy(saved, b, 28);
  int r = 1;
  for (int i = 0; i < NUM_BOXES; i++) {
    float* f = (float*)b;
    for (int k = 0; k < 6; k++) f[k] = BOXES[i][k] / 16.0f;
    b[24] = 1;
    r = orig_tess_block(self, block, pos, data);
  }
  memcpy(b, saved, 28);
  return r;
}

// Hook Thumb: 8 byte pertama fungsi diganti lompatan ke hook
static bool install_hook(uintptr_t target, void* hook, void** orig_out) {
  uint8_t* t = (uint8_t*)target;
  uint8_t* tr = (uint8_t*)mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (tr == (uint8_t*)MAP_FAILED) return false;
  memcpy(tr, t, 8);
  tr[8] = 0xDF; tr[9] = 0xF8; tr[10] = 0x00; tr[11] = 0xF0;  // ldr.w pc, [pc, #0]
  uint32_t back = (uint32_t)(target + 8) | 1;
  memcpy(tr + 12, &back, 4);
  __builtin___clear_cache((char*)tr, (char*)tr + 16);
  *orig_out = (void*)((uintptr_t)tr | 1);   // siap sebelum fungsi game ditimpa

  uintptr_t page = target & ~(uintptr_t)4095;
  if (mprotect((void*)page, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) return false;
  uint8_t patch[8] = {0xDF, 0xF8, 0x00, 0xF0, 0, 0, 0, 0};
  uint32_t h = (uint32_t)(uintptr_t)hook | 1;
  memcpy(patch + 4, &h, 4);
  memcpy(t, patch, 8);
  __builtin___clear_cache((char*)t, (char*)t + 8);
  mprotect((void*)page, 4096, PROT_READ | PROT_EXEC);
  return true;
}

static int find_cb(struct dl_phdr_info* info, size_t, void* data) {
  if (info->dlpi_name && strstr(info->dlpi_name, "libminecraftpe.so")) {
    *(uintptr_t*)data = (uintptr_t)info->dlpi_addr;
    return 1;
  }
  return 0;
}

static void* worker(void*) {
  uintptr_t base = 0;
  for (int i = 0; i < 600 && !base; i++) {   // tunggu sampai 60 detik
    dl_iterate_phdr(find_cb, &base);
    if (!base) usleep(100000);
  }
  if (!base) { LOG("libminecraftpe.so tidak ditemukan"); return nullptr; }
  uintptr_t target = base + OFF_TESS_BLOCK;
  if (memcmp((void*)target, SIG_TESS_BLOCK, 8) != 0) {
    LOG("tanda fungsi tidak cocok, hook dibatalkan");
    return nullptr;
  }
  fn_vertex_uv = (VertexUVFn)(base + OFF_VERTEX_UV);
  fn_quad = (QuadFn)(base + OFF_QUAD);
  fn_get_tex = (GetTexFn)(base + OFF_GET_TEXTURE);
  fn_color = (ColorFn)(base + OFF_COLOR);
  if (install_hook(target, (void*)hook_tess_block, (void**)&orig_tess_block)) {
    LOG("hook terpasang, base=0x%lx", (unsigned long)base);
  } else {
    LOG("hook gagal dipasang");
  }
  return nullptr;
}

__attribute__((constructor)) static void init() {
  LOG("pipe3d loaded");
  pthread_t th;
  pthread_create(&th, nullptr, worker, nullptr);
  pthread_detach(th);
}
