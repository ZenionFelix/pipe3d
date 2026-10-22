// Meja kerja 3D untuk Minecraft PE 0.15.10 (armeabi-v7a)
// + AIR PANTULAN (v13): pantulan bidang air seperti water shader Java (lihat bagian "AIR: PANTULAN"; pengaturan: pipe3d_water.txt).
// + GUNUNG TINGGI: hook Biome::setDepthAndScale menaikkan depth/scale biome gunung (config: pipe3d_biome.txt).
// + API UNGGUN (tahap 1): blok baru "campfire" (id 213) ikut sistem lentera.
// + LENTERA: blok baru "lantern" (item, gambar, inventory kreatif, bentuk kotak); aktif kalau pipe3d_lantern.txt berisi enabled=1.
// Versi 11 + PENGINTAI GL (tahap 1 dan 2 shadow map: hanya mencatat panggilan OpenGL, tidak mengubah tampilan).
// Tahap 3+4: peta bayangan matahari asli (lihat bagian "peta bayangan" di bawah). Pencatatan tahap 2b hanya jalan bila capture=1 di pipe3d_shadow.txt.
// Tahap 2b: tiap 45 detik (mulai 60 detik setelah game jalan, sampai 30 kali = sekitar 23 menit, cukup untuk satu siklus siang-malam)
// dicatat kotak kecil (matahari/bulan) beserta matriks dan posisi vertexnya, untuk menghitung arah matahari asli.
// Pencatatan pertama lengkap (urutan semua draw), berikutnya ringkas supaya log tidak membengkak.
// Versi 11: meja kerja (kotak) + tungku ke lapisan transparan + daun bentuk video (dua atap bersilang, dua lapis rapat, bidang penuh 16px miring 45 derajat, warna lebih terang) + log kecil.
#include <android/log.h>
#include <link.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <dlfcn.h>
#include <time.h>
#include <elf.h>
#include <math.h>
#include <stdarg.h>

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
// Bentuk daun (sesuai gambar acuan): satu "atap" = dua bidang miring bertemu di punggung atap.
// Tiap bidang berukuran penuh 1 blok (16 px) lalu dimiringkan 45 derajat, jadi naik 0.7071 dan
// menjorok 0.2071 keluar dari tepi blok di kedua sisi (ujung atap berbulu, sisi tidak polos).
// Dua atap disilangkan (searah X dan searah Z) jadi satu lapis; lapis itu disalin dan dinaikkan.
static const float LEAF_RISE = 0.70710678f;      // tinggi atap = jarak mendatar dari tepi ke punggung
static const float LEAF_LIFT = 0.50f;   // lapis kedua dinaikkan 0.5: jarak antar lapis merata (0, 0.5, 1.0, 1.5 ...) sehingga rapat dan menyambung saat ditumpuk
static const float LEAF_BRIGHT = 1.30f; // pengali kecerahan warna daun (supaya bagian gelap tidak terlalu hitam)
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


static int clamp255(float v) { return v < 0 ? 0 : (v > 255 ? 255 : (int)v); }
static void set_shaded_color(void* tess, int c, float k) {
  k *= LEAF_BRIGHT;
  int r = clamp255(((c >> 16) & 255) * k);
  int g = clamp255(((c >> 8) & 255) * k);
  int b = clamp255((c & 255) * k);
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
  const float R = LEAF_RISE;
  const float ov = R - 0.5f;              // seberapa jauh ujung atap menjorok ke luar blok
  for (int layer = 0; layer < 2; layer++) {
    float y0 = layer ? LEAF_LIFT : 0.0f;
    float y1 = y0 + R;
    // atap searah X: dua bidang, punggung atap di z = 0.5
    const float a1[4][3] = {{0, y0, -ov}, {1, y0, -ov}, {1, y1, 0.5f}, {0, y1, 0.5f}};
    const float a2[4][3] = {{0, y0, 1 + ov}, {1, y0, 1 + ov}, {1, y1, 0.5f}, {0, y1, 0.5f}};
    // atap searah Z: dua bidang, punggung atap di x = 0.5
    const float b1[4][3] = {{-ov, y0, 0}, {-ov, y0, 1}, {0.5f, y1, 1}, {0.5f, y1, 0}};
    const float b2[4][3] = {{1 + ov, y0, 0}, {1 + ov, y0, 1}, {0.5f, y1, 1}, {0.5f, y1, 0}};
    // beda terang-gelap antar bidang dibuat tipis supaya tidak ada bidang yang tampak hitam
    set_shaded_color(tess, tint, 1.00f); draw_quad(tess, bx, by, bz, a1, uv);
    set_shaded_color(tess, tint, 0.94f); draw_quad(tess, bx, by, bz, a2, uv);
    set_shaded_color(tess, tint, 0.97f); draw_quad(tess, bx, by, bz, b1, uv);
    set_shaded_color(tess, tint, 0.90f); draw_quad(tess, bx, by, bz, b2, uv);
  }
  return true;
}

// Lentera, rantai, lentera gantung: id diisi saat pendaftaran blok, -1 = belum ada
#define LT_COUNT 4
static int g_lt_ids[LT_COUNT] = {-1, -1, -1, -1};
static int g_lantern_shape = 0;
// Model tiap blok dalam kotak (satuan 1/16 blok): x0,y0,z0,x1,y1,z1
struct LtModel { int n; float b[6][6]; };
static const LtModel LT_MODELS[LT_COUNT] = {
  {3, {{5, 0, 5, 11, 7, 11}, {6, 7, 6, 10, 9, 10}, {7, 9, 7, 9, 10, 9}}},       // lentera berdiri: badan, tutup, cincin
  {2, {{6, 0, 7, 10, 16, 9}, {7, 0, 6, 9, 16, 10}}},                            // rantai: dua bidang menyilang
  {3, {{5, 6, 5, 11, 13, 11}, {6, 13, 6, 10, 15, 10}, {7, 15, 7, 9, 16, 9}}},   // lentera gantung: tangkai sampai atas blok
  {5, {{0, 0, 2, 16, 4, 6}, {0, 0, 10, 16, 4, 14},                               // api unggun: dua kayu searah X (bawah)
       {2, 4, 0, 6, 7, 16}, {10, 4, 0, 14, 7, 16},                               //   dua kayu searah Z (atas, menyilang)
       {5, 0, 5, 11, 3, 11}}},                                                   //   dasar bara di tengah
};

static void fire_record(int x, int y, int z);   // daftar posisi api unggun untuk efek asap (lihat bagian API UNGGUN)
static void fire_trace(const char* tag, int a, int b, int c);   // jejak diagnosa crash unggun (maks 60 baris)

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
  if (g_lantern_shape) {
    for (int m = 0; m < LT_COUNT; m++) {
      if (g_lt_ids[m] < 0 || id != g_lt_ids[m]) continue;
      if (m == 3 && pos) { int* fp = (int*)pos; fire_trace("tess mulai", fp[0], fp[1], fp[2]); fire_record(fp[0], fp[1], fp[2]); }   // api unggun: catat posisinya
      uint8_t* lb = (uint8_t*)self + BOUNDS_OFF;
      uint8_t lsaved[28];
      memcpy(lsaved, lb, 28);
      int lr = 1;
      for (int i = 0; i < LT_MODELS[m].n; i++) {
        float* f = (float*)lb;
        for (int k = 0; k < 6; k++) f[k] = LT_MODELS[m].b[i][k] / 16.0f;
        lb[24] = 1;
        lr = orig_tess_block(self, block, pos, data);
      }
      memcpy(lb, lsaved, 28);
      if (m == 3) fire_trace("tess selesai", 0, 0, 0);
      return lr;
    }
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


// ======================= Pengintai OpenGL (tahap 1 shadow map) =======================
// Mengganti isi tabel impor (GOT) milik libminecraftpe.so untuk beberapa fungsi GL/EGL, hanya untuk MENCATAT.
// Hasil ada di /sdcard/games/com.mojang/pipe3d_log.txt (ringkasan tiap 300 frame, 4 kali).
typedef unsigned int GLuint_t;
typedef void (*fn_useprog_t)(GLuint_t);
typedef void (*fn_drawel_t)(unsigned, int, unsigned, const void*);
typedef void (*fn_drawar_t)(unsigned, int, int);
typedef void (*fn_bindfb_t)(unsigned, GLuint_t);
typedef void (*fn_viewport_t)(int, int, int, int);
typedef unsigned (*fn_swap_t)(void*, void*);
static fn_useprog_t o_useprog = nullptr;
static fn_drawel_t o_drawel = nullptr;
static fn_drawar_t o_drawar = nullptr;
static fn_bindfb_t o_bindfb = nullptr;
static fn_viewport_t o_viewport = nullptr;
static fn_swap_t o_swap = nullptr;

struct ProgStat { unsigned id; unsigned draws; unsigned verts; unsigned mode; unsigned type; bool described; int kind;
  int loc_origin, loc_wv, loc_proj, loc_wvp, loc_cc, loc_vp, loc_t2; float vp_orig[3]; int vp_state; int loc_world; };   // kind: 0 belum dicek, 1 terrain, 2 bukan
static ProgStat g_prog[48];
static int g_np = 0;
static unsigned g_cur_prog = 0, g_cur_fbo = 0;
static unsigned g_fbo_binds = 0, g_vp_calls = 0;
static int g_vp_w[8], g_vp_h[8]; static unsigned g_vp_n[8]; static int g_nvp = 0;
static unsigned g_frames = 0, g_summaries = 0;
static bool g_gl_info_done = false;

static ProgStat* prog_stat(unsigned id) {
  for (int i = 0; i < g_np; i++) if (g_prog[i].id == id) return &g_prog[i];
  if (g_np >= 48) return &g_prog[47];
  ProgStat* p = &g_prog[g_np++];
  memset(p, 0, sizeof(*p)); p->id = id;
  return p;
}

// ---- Tahap 2: pencatatan rinci draw terrain (hanya membaca status GL, tidak mengubah apa pun) ----
typedef int (*gl_loc_t)(unsigned, const char*);
typedef void (*gl_unifv_t)(unsigned, int, float*);
typedef void (*gl_unifiv_t)(unsigned, int, int*);
typedef void (*gl_vaiv_t)(unsigned, unsigned, int*);
typedef void (*gl_vaptr_t)(unsigned, unsigned, void**);
typedef unsigned char (*gl_isen_t)(unsigned);
typedef void (*gl_getb_t)(unsigned, unsigned char*);
typedef void (*gl_geti_t)(unsigned, int*);
typedef void* (*gl_maprange_t)(unsigned, intptr_t, intptr_t, unsigned);
typedef unsigned char (*gl_unmap_t)(unsigned);
static void* g_glib = nullptr;
static gl_loc_t f_getuloc = nullptr, f_getaloc = nullptr;
static gl_unifv_t f_unifv = nullptr;
static gl_unifiv_t f_unifiv = nullptr;
static gl_vaiv_t f_vaiv = nullptr;
static gl_vaptr_t f_vaptr = nullptr;
static gl_isen_t f_isen = nullptr;
static gl_getb_t f_getb = nullptr;
static gl_geti_t f_geti = nullptr;
static gl_maprange_t f_maprange = nullptr;
static gl_unmap_t f_unmap = nullptr;
static unsigned g_seq = 0, g_cap_other = 0;
static bool g_cap_full = true;
static unsigned g_cap_count = 0;
static double g_next_cap = 0;
static double mono_sec() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}
static bool g_capturing = false, g_cap_mat = false;
static unsigned g_cap_draws = 0;
static FILE* g_cap_f = nullptr;

static void* glib() { if (!g_glib) g_glib = dlopen("libGLESv2.so", RTLD_NOW); return g_glib; }
static void cap_init() {
  void* L = glib();
  if (!L || f_geti) return;
  f_getuloc = (gl_loc_t)dlsym(L, "glGetUniformLocation");
  f_getaloc = (gl_loc_t)dlsym(L, "glGetAttribLocation");
  f_unifv = (gl_unifv_t)dlsym(L, "glGetUniformfv");
  f_unifiv = (gl_unifiv_t)dlsym(L, "glGetUniformiv");
  f_vaiv = (gl_vaiv_t)dlsym(L, "glGetVertexAttribiv");
  f_vaptr = (gl_vaptr_t)dlsym(L, "glGetVertexAttribPointerv");
  f_isen = (gl_isen_t)dlsym(L, "glIsEnabled");
  f_getb = (gl_getb_t)dlsym(L, "glGetBooleanv");
  f_geti = (gl_geti_t)dlsym(L, "glGetIntegerv");
  f_maprange = (gl_maprange_t)dlsym(L, "glMapBufferRange");
  f_unmap = (gl_unmap_t)dlsym(L, "glUnmapBuffer");
}

static void cap_print_uniform(FILE* f, unsigned prog, const char* name, int n) {
  int loc = f_getuloc ? f_getuloc(prog, name) : -1;
  if (loc < 0 || !f_unifv) { fprintf(f, "  %s: tidak ada\n", name); return; }
  float v[16] = {0};
  f_unifv(prog, loc, v);
  fprintf(f, "  %s:", name);
  for (int i = 0; i < n; i++) fprintf(f, " %g", v[i]);
  fprintf(f, "\n");
}

static void capture_draw(ProgStat* s, unsigned mode, int count, unsigned type, const void* idx) {
  cap_init();
  if (!g_cap_f || !f_geti || !f_isen || !f_getb) return;
  unsigned n = g_cap_draws++;
  if (n >= (g_cap_full ? 4u : 1u)) return;
  FILE* f = g_cap_f;
  unsigned id = s->id;
  int vbo = 0, ibo = 0, fbo = 0, vp[4] = {0, 0, 0, 0};
  unsigned char dm = 0;
  f_geti(0x8894, &vbo); f_geti(0x8895, &ibo); f_geti(0x8CA6, &fbo); f_geti(0x0BA2, vp); f_getb(0x0B72, &dm);
  fprintf(f, "T2 draw#%u prog=%u mode=0x%x count=%d type=0x%x idx=%p vbo=%d ibo=%d fbo=%d vp=%d,%d,%d,%d blend=%d cull=%d depth=%d dmask=%d\n",
          n, id, mode, count, type, idx, vbo, ibo, fbo, vp[0], vp[1], vp[2], vp[3],
          (int)f_isen(0x0BE2), (int)f_isen(0x0B44), (int)f_isen(0x0B71), (int)dm);
  static const char* an[4] = {"POSITION", "COLOR", "TEXCOORD_0", "TEXCOORD_1"};
  for (int i = 0; i < 4; i++) {
    int loc = f_getaloc ? f_getaloc(id, an[i]) : -1;
    if (loc < 0 || !f_vaiv || !f_vaptr) { fprintf(f, "  attr %s: tidak ada\n", an[i]); continue; }
    int en = 0, sz = 0, ty = 0, nm = 0, st = 0, bb = 0; void* po = nullptr;
    f_vaiv(loc, 0x8622, &en); f_vaiv(loc, 0x8623, &sz); f_vaiv(loc, 0x8625, &ty);
    f_vaiv(loc, 0x886A, &nm); f_vaiv(loc, 0x8624, &st); f_vaiv(loc, 0x889F, &bb);
    f_vaptr(loc, 0x8645, &po);
    fprintf(f, "  attr %s loc=%d en=%d size=%d type=0x%x norm=%d stride=%d off=%p buf=%d\n", an[i], loc, en, sz, ty, nm, st, po, bb);
  }
  cap_print_uniform(f, id, "CHUNK_ORIGIN_AND_SCALE", 4);
  cap_print_uniform(f, id, "VIEW_POS", 3);
  cap_print_uniform(f, id, "FOG_CONTROL", 2);
  if (f_getuloc && f_unifiv) {
    int t0 = -1, t1 = -1, l0 = f_getuloc(id, "TEXTURE_0"), l1 = f_getuloc(id, "TEXTURE_1");
    if (l0 >= 0) f_unifiv(id, l0, &t0);
    if (l1 >= 0) f_unifiv(id, l1, &t1);
    fprintf(f, "  TEXTURE_0 unit=%d, TEXTURE_1 unit=%d\n", t0, t1);
  }
  if (!g_cap_mat) {   // matriks cukup sekali per frame
    g_cap_mat = true;
    cap_print_uniform(f, id, "WORLDVIEW", 16);
    cap_print_uniform(f, id, "PROJ", 16);
  }
}


// Cetak satu atribut vertex beserta (kalau terbaca) beberapa vertex pertama
static void dump_attr(FILE* f, unsigned prog, const char* name, int nv) {
  int loc = f_getaloc ? f_getaloc(prog, name) : -1;
  if (loc < 0 || !f_vaiv || !f_vaptr) { fprintf(f, "  %s: tidak ada\n", name); return; }
  int en = 0, sz = 0, ty = 0, nm = 0, st = 0, bb = 0; void* po = nullptr;
  f_vaiv(loc, 0x8622, &en); f_vaiv(loc, 0x8623, &sz); f_vaiv(loc, 0x8625, &ty);
  f_vaiv(loc, 0x886A, &nm); f_vaiv(loc, 0x8624, &st); f_vaiv(loc, 0x889F, &bb);
  f_vaptr(loc, 0x8645, &po);
  fprintf(f, "  %s loc=%d en=%d size=%d type=0x%x norm=%d stride=%d off=%p buf=%d\n", name, loc, en, sz, ty, nm, st, po, bb);
  if (ty != 0x1406 || sz < 2 || sz > 4 || !en || nv < 1 || nv > 4) return;
  int stride = st ? st : sz * 4;
  float v[4][4] = {{0}};
  bool ok = false;
  if (bb == 0 && po) {   // array di memori biasa
    for (int i = 0; i < nv; i++) memcpy(v[i], (const uint8_t*)po + i * stride, sz * 4);
    ok = true;
  } else if (bb != 0 && f_maprange && f_unmap) {   // buffer GL: hanya dibaca kalau memang sedang terikat
    int cur = 0; f_geti(0x8894, &cur);
    if (cur == bb) {
      intptr_t off = (intptr_t)po, len = (intptr_t)((nv - 1) * stride + sz * 4);
      void* m = f_maprange(0x8892, off, len, 1);   // GL_MAP_READ_BIT
      if (m) {
        for (int i = 0; i < nv; i++) memcpy(v[i], (const uint8_t*)m + i * stride, sz * 4);
        f_unmap(0x8892);
        ok = true;
      }
    }
  }
  if (!ok) { fprintf(f, "  (data vertex tidak terbaca)\n"); return; }
  for (int i = 0; i < nv; i++) {
    fprintf(f, "  v%d=(", i);
    for (int k = 0; k < sz; k++) fprintf(f, k ? " %g" : "%g", v[i][k]);
    fprintf(f, ")\n");
  }
}

// Catat urutan semua draw; kotak kecil (4 atau 6 vertex/indeks) di luar terrain dicatat rinci
static void capture_other(ProgStat* s, unsigned mode, int count, bool detail) {
  cap_init();
  if (!g_cap_f || !f_geti || !f_isen || !f_getb) return;
  unsigned n = g_seq++;
  if (n >= 300) return;
  FILE* f = g_cap_f;
  int fbo = 0, tex = 0, act = 0;
  unsigned char dm = 0;
  f_geti(0x8CA6, &fbo); f_geti(0x8069, &tex); f_geti(0x84E0, &act); f_getb(0x0B72, &dm);
  unsigned cap_limit = g_cap_full ? 20u : 12u;
  bool want = detail && (count == 6 || count == 4) && fbo == 0 && g_cap_other < cap_limit;
  if (g_cap_full || want)
    fprintf(f, "S seq=%u prog=%u mode=0x%x count=%d fbo=%d blend=%d depth=%d dmask=%d cull=%d tex=%d act=0x%x\n",
            n, s->id, mode, count, fbo, (int)f_isen(0x0BE2), (int)f_isen(0x0B71), (int)dm, (int)f_isen(0x0B44), tex, act);
  if (want) {
    g_cap_other++;
    cap_print_uniform(f, s->id, "WORLDVIEWPROJ", 16);
    cap_print_uniform(f, s->id, "CURRENT_COLOR", 4);
    dump_attr(f, s->id, "POSITION", 4);
    if (g_cap_full) dump_attr(f, s->id, "TEXCOORD_0", 4);
  }
}

// ======================= Tahap 3 + 4: peta bayangan matahari =======================
// Alur: tiap frame, semua draw terrain direkam. Tepat sebelum eglSwapBuffers, terrain digambar ulang dari arah matahari
// ke tekstur kedalaman. Frame berikutnya shader terrain (renderchunk.fragment) membaca peta itu lewat uniform SHADOW_*.
// Arah matahari dibaca dari kotak matahari/bulan yang digambar game (tahap 2b).
// Pengaturan opsional di /sdcard/games/com.mojang/pipe3d_shadow.txt (baris "kunci=nilai"): on=1 level=3 size=1024 capture=0
// (radius 56 dan bias dikunci di shader renderchunk.vertex/.fragment)
#define GLX_LIST(X) \
  X(void, GenFramebuffers, (int, unsigned*)) \
  X(void, BindFramebuffer, (unsigned, unsigned)) \
  X(void, FramebufferTexture2D, (unsigned, unsigned, unsigned, unsigned, int)) \
  X(unsigned, CheckFramebufferStatus, (unsigned)) \
  X(void, GenTextures, (int, unsigned*)) \
  X(void, BindTexture, (unsigned, unsigned)) \
  X(void, TexImage2D, (unsigned, int, int, int, int, int, unsigned, unsigned, const void*)) \
  X(void, TexParameteri, (unsigned, unsigned, int)) \
  X(void, ActiveTexture, (unsigned)) \
  X(void, Viewport, (int, int, int, int)) \
  X(void, Clear, (unsigned)) \
  X(void, ClearDepthf, (float)) \
  X(void, DepthMask, (unsigned char)) \
  X(void, DepthFunc, (unsigned)) \
  X(void, Enable, (unsigned)) \
  X(void, Disable, (unsigned)) \
  X(void, ColorMask, (unsigned char, unsigned char, unsigned char, unsigned char)) \
  X(void, CullFace, (unsigned)) \
  X(void, PolygonOffset, (float, float)) \
  X(void, UseProgram, (unsigned)) \
  X(void, BindBuffer, (unsigned, unsigned)) \
  X(void, VertexAttribPointer, (unsigned, int, unsigned, unsigned char, int, const void*)) \
  X(void, EnableVertexAttribArray, (unsigned)) \
  X(void, DisableVertexAttribArray, (unsigned)) \
  X(void, UniformMatrix4fv, (int, int, unsigned char, const float*)) \
  X(void, Uniform1i, (int, int)) \
    X(void, Uniform3f, (int, float, float, float)) \
  X(void, Uniform4f, (int, float, float, float, float)) \
  X(void, DrawElements, (unsigned, int, unsigned, const void*)) \
  X(unsigned, CreateShader, (unsigned)) \
  X(void, ShaderSource, (unsigned, int, const char* const*, const int*)) \
  X(void, CompileShader, (unsigned)) \
  X(void, GetShaderiv, (unsigned, unsigned, int*)) \
  X(void, GetShaderInfoLog, (unsigned, int, int*, char*)) \
  X(unsigned, CreateProgram, (void)) \
  X(void, AttachShader, (unsigned, unsigned)) \
  X(void, BindAttribLocation, (unsigned, unsigned, const char*)) \
  X(void, LinkProgram, (unsigned)) \
  X(void, GetProgramiv, (unsigned, unsigned, int*)) \
  X(void, GetProgramInfoLog, (unsigned, int, int*, char*)) \
  X(unsigned, GetError, (void)) \
  X(void, GetFloatv, (unsigned, float*)) \
  X(void, GetBufferParameteriv, (unsigned, unsigned, int*)) \
  X(void, GenBuffers, (int, unsigned*)) \
  X(void, BufferData, (unsigned, intptr_t, const void*, unsigned)) \
  X(void, DrawArrays, (unsigned, int, int)) \
  X(void, Uniform1f, (int, float)) \
  X(void, Uniform2f, (int, float, float)) \
  X(void, FrontFace, (unsigned)) \
  X(void, BlendFuncSeparate, (unsigned, unsigned, unsigned, unsigned)) \
  X(void, ClearColor, (float, float, float, float)) \
  X(void, GenRenderbuffers, (int, unsigned*)) \
  X(void, BindRenderbuffer, (unsigned, unsigned)) \
  X(void, RenderbufferStorage, (unsigned, unsigned, int, int)) \
  X(void, FramebufferRenderbuffer, (unsigned, unsigned, unsigned, unsigned)) \
  X(void, DeleteProgram, (unsigned))

struct GLX {
#define X(ret, name, args) ret (*name) args;
  GLX_LIST(X)
#undef X
};
static GLX gx;
static bool g_gx_ok = false;
typedef void (*gl_drawbuffers_t)(int, const unsigned*);
typedef void (*gl_readbuffer_t)(unsigned);
static gl_drawbuffers_t gx_drawbuffers = nullptr;
static gl_readbuffer_t gx_readbuffer = nullptr;

static void gx_init() {
  void* L = glib();
  if (!L) return;
  bool ok = true;
#define X(ret, name, args) gx.name = (ret (*) args)dlsym(L, "gl" #name); if (!gx.name) { LOG("shadow: gl%s tidak ada", #name); ok = false; }
  GLX_LIST(X)
#undef X
  gx_drawbuffers = (gl_drawbuffers_t)dlsym(L, "glDrawBuffers");
  gx_readbuffer = (gl_readbuffer_t)dlsym(L, "glReadBuffer");
  g_gx_ok = ok;
}

// ---- pengaturan ----
static int g_cfg_on = 1, g_cfg_size = 1024, g_cfg_capture = 0, g_cfg_level = -1, g_cfg_playerbox = 1;
static int g_cfg_guard = 1;   // guard=0 di pipe3d_shadow.txt mematikan pengaman crash (level bayangan tidak pernah turun sendiri)
static float g_cfg_radius = 56.0f, g_cfg_bias = 0.12f;
static bool g_cfg_loaded = false;
static void cfg_load() {
  if (g_cfg_loaded) return;
  g_cfg_loaded = true;
  FILE* f = fopen("/sdcard/games/com.mojang/pipe3d_shadow.txt", "r");
  if (!f) return;
  char line[128];
  while (fgets(line, sizeof(line), f)) {
    int iv; float fv;
    if (sscanf(line, "on=%d", &iv) == 1) g_cfg_on = iv;
    else if (sscanf(line, "size=%d", &iv) == 1) g_cfg_size = (iv >= 256 && iv <= 4096) ? iv : 1024;
    else if (sscanf(line, "capture=%d", &iv) == 1) g_cfg_capture = iv;
    else if (sscanf(line, "level=%d", &iv) == 1) g_cfg_level = (iv < 0) ? 0 : (iv > 4 ? 4 : iv);
    else if (sscanf(line, "playerbox=%d", &iv) == 1) g_cfg_playerbox = iv;
    else if (sscanf(line, "guard=%d", &iv) == 1) g_cfg_guard = iv;
    else if (sscanf(line, "radius=%f", &fv) == 1) { (void)fv; }   // radius dikunci 56 (sama dengan konstanta di shader)
    else if (sscanf(line, "bias=%f", &fv) == 1) { (void)fv; }     // bias dikunci di shader
  }
  fclose(f);
}
static void sh_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void sh_log(const char* fmt, ...) {
  FILE* f = fopen("/sdcard/games/com.mojang/pipe3d_log.txt", "a");
  if (!f) return;
  va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
  fclose(f);
}


// ---- pengaman crash ----
// Pengaman bisa dimatikan: tulis "guard=0" di pipe3d_shadow.txt.
// level 0: mati; 1: hanya mengenali arah matahari; 2: + menggambar peta bayangan (belum dipakai shader); 3: bayangan terrain; 4: + bayangan mob dan pemain.
// Pada frame terrain pertama, file status ditandai "armed=1". Kalau game crash atau macet sebelum 90 frame terrain berlalu,
// peluncuran berikutnya otomatis turun satu level. Untuk memaksa level tertentu: tulis "level=N" di pipe3d_shadow.txt.
static const char* GUARD_FILE = "/sdcard/games/com.mojang/pipe3d_state2.txt";
static int g_level = 0;
static bool g_level_inited = false, g_guard_done = false, g_guard_armed = false;
static unsigned g_guard_frames = 0;
static int g_bc = 0;
static void bc(const char* m) { if (g_bc < 40) { g_bc++; sh_log("langkah: %s (frame %u)\n", m, g_frames); } }
static void guard_write(int lvl, int armed) {
  FILE* f = fopen(GUARD_FILE, "w");
  if (f) { fprintf(f, "lvl=%d\narmed=%d\n", lvl, armed); fclose(f); }
}
static void level_init() {
  g_level_inited = true;
  cfg_load();
  int lvl = 4, armed = 0; bool have = false;
  FILE* f = fopen(GUARD_FILE, "r");
  if (f) {
    char line[64]; int v;
    while (fgets(line, sizeof(line), f)) {
      if (sscanf(line, "lvl=%d", &v) == 1) { lvl = v; have = true; }
      else if (sscanf(line, "armed=%d", &v) == 1) armed = v;
    }
    fclose(f);
  }
  if (g_cfg_level >= 0) { lvl = g_cfg_level; armed = 0; sh_log("pipe3d: level dipaksa lewat pengaturan = %d\n", lvl); }
  else if (!g_cfg_guard) { sh_log("pipe3d: pengaman crash dimatikan (guard=0); level selalu 4, level lama di file status (%d) diabaikan\n", have ? lvl : 4); lvl = 4; armed = 0; }
  else if (have && armed) { sh_log("pipe3d: peluncuran sebelumnya tidak lolos 90 frame terrain pada level %d, turun ke level %d\n", lvl, lvl > 0 ? lvl - 1 : 0); lvl = lvl > 0 ? lvl - 1 : 0; }
  if (lvl < 0) lvl = 0;
  if (lvl > 4) lvl = 4;
  if (!g_cfg_on) lvl = 0;
  g_level = lvl;
  guard_write(g_level, 0);
  sh_log("pipe3d: bayangan level=%d\n", g_level);
}
static void guard_arm() {
  g_guard_done = true;
  if (g_level >= 1 && g_cfg_guard) { guard_write(g_level, 1); g_guard_armed = true; }   // guard=0: tidak ditandai, jadi tidak pernah menurunkan level
  bc("frame terrain pertama, pengaman aktif");
}

// ---- sumber daya peta bayangan ----
static int g_sh_state = 0;   // 0 belum diinisialisasi, 1 siap, -1 gagal/nonaktif
static unsigned g_sh_tex = 0, g_sh_fbo = 0, g_sh_prog = 0, g_sh_ctex = 0;
static int g_sh_loc_lvp = -1, g_sh_loc_org = -1, g_sh_loc_tex = -1;
static int g_sh_size = 1024;
static unsigned g_eh_prog = 0, g_cube_vbo = 0, g_cube_ibo = 0;
static int g_eh_loc = -1;
static const float SH_DEPTH_D = 160.0f;   // jarak mata cahaya dari pusat; kedalaman total = 2 * D

static const char* SH_VS =
  "attribute vec3 POSITION;\n"
  "attribute vec2 TEXCOORD_0;\n"
  "uniform mat4 LVP;\n"
  "uniform vec3 ORG;\n"
  "varying vec2 vuv;\n"
  "void main() {\n"
  "  vuv = TEXCOORD_0;\n"
  "  gl_Position = LVP * vec4(POSITION + ORG, 1.0);\n"
  "}\n";
static const char* SH_FS =
  "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
  "precision highp float;\n"
  "#else\n"
  "precision mediump float;\n"
  "#endif\n"
  "varying vec2 vuv;\n"
  "uniform sampler2D TEX;\n"
  "void main() {\n"
  "  if (texture2D(TEX, vuv).a < 0.5) discard;\n"
  "  gl_FragColor = vec4(1.0);\n"
  "}\n";

static void clear_gl_errors() { for (int i = 0; i < 16; i++) if (gx.GetError() == 0) break; }

static const char* EH_VS =
  "attribute vec4 POSITION;\n"
  "uniform mat4 LVPM;\n"
  "void main() { gl_Position = LVPM * vec4(POSITION.xyz, 1.0); }\n";
static const char* EH_FS =
  "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
  "precision highp float;\n"
  "#else\n"
  "precision mediump float;\n"
  "#endif\n"
  "void main() { gl_FragColor = vec4(1.0); }\n";

static unsigned sh_compile(unsigned type, const char* src) {
  unsigned sh = gx.CreateShader(type);
  gx.ShaderSource(sh, 1, &src, nullptr);
  gx.CompileShader(sh);
  int ok = 0; gx.GetShaderiv(sh, 0x8B81, &ok);
  if (!ok) { char buf[512] = {0}; int n = 0; gx.GetShaderInfoLog(sh, 511, &n, buf); sh_log("shadow: shader gagal dikompilasi: %s\n", buf); return 0; }
  return sh;
}

static void shadow_init() {
  g_sh_state = -1;
  cfg_load();
  cap_init();
  if (!g_cfg_on) { sh_log("shadow: dimatikan lewat pengaturan (on=0)\n"); return; }
  gx_init();
  if (!g_gx_ok || !f_geti || !f_isen || !f_getb || !f_unifv || !f_getuloc || !f_vaiv || !f_vaptr) { sh_log("shadow: fungsi GL tidak lengkap\n"); return; }
  g_sh_size = g_cfg_size;
  int prev_fbo = 0, prev_tex = 0;
  f_geti(0x8CA6, &prev_fbo); f_geti(0x8069, &prev_tex);
  clear_gl_errors();
  // tekstur kedalaman
  gx.GenTextures(1, &g_sh_tex);
  gx.BindTexture(0x0DE1, g_sh_tex);
  gx.TexImage2D(0x0DE1, 0, 0x81A6, g_sh_size, g_sh_size, 0, 0x1902, 0x1405, nullptr);   // DEPTH_COMPONENT24
  unsigned err = gx.GetError();
  if (err != 0) {
    gx.TexImage2D(0x0DE1, 0, 0x81A5, g_sh_size, g_sh_size, 0, 0x1902, 0x1403, nullptr); // DEPTH_COMPONENT16
    err = gx.GetError();
    sh_log("shadow: depth24 gagal, mencoba depth16 (err=0x%x)\n", err);
  }
  gx.TexParameteri(0x0DE1, 0x2801, 0x2600);   // MIN_FILTER = NEAREST
  gx.TexParameteri(0x0DE1, 0x2800, 0x2600);   // MAG_FILTER = NEAREST
  gx.TexParameteri(0x0DE1, 0x2802, 0x812F);   // WRAP_S = CLAMP_TO_EDGE
  gx.TexParameteri(0x0DE1, 0x2803, 0x812F);   // WRAP_T
  // framebuffer
  gx.GenFramebuffers(1, &g_sh_fbo);
  gx.BindFramebuffer(0x8D40, g_sh_fbo);
  gx.FramebufferTexture2D(0x8D40, 0x8D00, 0x0DE1, g_sh_tex, 0);   // DEPTH_ATTACHMENT
  if (gx_drawbuffers) { unsigned none = 0; gx_drawbuffers(1, &none); }
  if (gx_readbuffer) gx_readbuffer(0);
  unsigned status = gx.CheckFramebufferStatus(0x8D40);
  sh_log("shadow: framebuffer hanya-kedalaman status=0x%x (0x8cd5 = lengkap)\n", status);
  if (status != 0x8CD5) {   // cadangan: tambah lampiran warna
    gx.GenTextures(1, &g_sh_ctex);
    gx.BindTexture(0x0DE1, g_sh_ctex);
    gx.TexImage2D(0x0DE1, 0, 0x1908, g_sh_size, g_sh_size, 0, 0x1908, 0x1401, nullptr);
    gx.TexParameteri(0x0DE1, 0x2801, 0x2600);
    gx.TexParameteri(0x0DE1, 0x2800, 0x2600);
    gx.FramebufferTexture2D(0x8D40, 0x8CE0, 0x0DE1, g_sh_ctex, 0);
    if (gx_drawbuffers) { unsigned c0 = 0x8CE0; gx_drawbuffers(1, &c0); }
    status = gx.CheckFramebufferStatus(0x8D40);
    sh_log("shadow: framebuffer + warna status=0x%x\n", status);
  }
  gx.BindFramebuffer(0x8D40, (unsigned)prev_fbo);
  gx.BindTexture(0x0DE1, (unsigned)prev_tex);
  if (status != 0x8CD5) { sh_log("shadow: framebuffer tidak lengkap, bayangan dinonaktifkan\n"); return; }
  // program bayangan
  unsigned vs = sh_compile(0x8B31, SH_VS), fs = sh_compile(0x8B30, SH_FS);
  if (!vs || !fs) return;
  g_sh_prog = gx.CreateProgram();
  gx.AttachShader(g_sh_prog, vs); gx.AttachShader(g_sh_prog, fs);
  gx.BindAttribLocation(g_sh_prog, 0, "POSITION");
  gx.BindAttribLocation(g_sh_prog, 1, "TEXCOORD_0");
  gx.LinkProgram(g_sh_prog);
  int linked = 0; gx.GetProgramiv(g_sh_prog, 0x8B82, &linked);
  if (!linked) { char buf[512] = {0}; int n = 0; gx.GetProgramInfoLog(g_sh_prog, 511, &n, buf); sh_log("shadow: link gagal: %s\n", buf); return; }
  g_sh_loc_lvp = f_getuloc(g_sh_prog, "LVP");
  g_sh_loc_org = f_getuloc(g_sh_prog, "ORG");
  g_sh_loc_tex = f_getuloc(g_sh_prog, "TEX");
  {   // program bayangan untuk mob dan kotak pemain (tanpa tekstur); kalau gagal, hanya fitur ini yang mati
    unsigned evs = sh_compile(0x8B31, EH_VS), efs = sh_compile(0x8B30, EH_FS);
    if (evs && efs) {
      g_eh_prog = gx.CreateProgram();
      gx.AttachShader(g_eh_prog, evs); gx.AttachShader(g_eh_prog, efs);
      gx.BindAttribLocation(g_eh_prog, 0, "POSITION");
      gx.LinkProgram(g_eh_prog);
      int el = 0; gx.GetProgramiv(g_eh_prog, 0x8B82, &el);
      if (!el) { sh_log("shadow: program mob gagal di-link\n"); g_eh_prog = 0; }
      else g_eh_loc = f_getuloc(g_eh_prog, "LVPM");
    }
    static const float cv[24] = {-0.5f, -0.5f, -0.5f, 0.5f, -0.5f, -0.5f, 0.5f, 0.5f, -0.5f, -0.5f, 0.5f, -0.5f,
                                 -0.5f, -0.5f, 0.5f, 0.5f, -0.5f, 0.5f, 0.5f, 0.5f, 0.5f, -0.5f, 0.5f, 0.5f};
    static const unsigned short ci[36] = {0, 1, 2, 0, 2, 3, 4, 6, 5, 4, 7, 6, 0, 4, 5, 0, 5, 1, 3, 2, 6, 3, 6, 7, 0, 3, 7, 0, 7, 4, 1, 5, 6, 1, 6, 2};
    int pa = 0, pe = 0; f_geti(0x8894, &pa); f_geti(0x8895, &pe);
    gx.GenBuffers(1, &g_cube_vbo); gx.BindBuffer(0x8892, g_cube_vbo); gx.BufferData(0x8892, (intptr_t)sizeof(cv), cv, 0x88E4);
    gx.GenBuffers(1, &g_cube_ibo); gx.BindBuffer(0x8893, g_cube_ibo); gx.BufferData(0x8893, (intptr_t)sizeof(ci), ci, 0x88E4);
    gx.BindBuffer(0x8892, (unsigned)pa); gx.BindBuffer(0x8893, (unsigned)pe);
  }
  clear_gl_errors();
  g_sh_state = 1;
  sh_log("shadow: siap. ukuran=%d radius=%.0f bias=%.2f\n", g_sh_size, g_cfg_radius, g_cfg_bias);
}

// ---- status per frame ----
struct DrawRec { int vbo, ibo, count, vsz, isz; unsigned type; uintptr_t idx; float o[3]; unsigned pid; float sc; };
static DrawRec g_dl[2][2048];
static int g_dn[2] = {0, 0};
static int g_dcur = 0;
static unsigned g_tframe = 0xFFFFFFFFu;   // nomor frame terakhir yang punya draw terrain
static bool g_delta_ok = false, g_delta_exact = false, g_have_prev_delta = false;
static int g_en = 0;   // jumlah draw mob yang direkam di frame ini (daftar g_el ada di bagian mob)
static float g_last_delta[3] = {0, 0, 0};
static float g_delta[3] = {0, 0, 0};
static unsigned g_delta_ver = 0;
static float g_wv[16], g_proj[16];
static bool g_view_ok = false;
static int g_atlas_tex = 0, g_prev_unit4 = 0;
static bool g_unit4_saved = false;
static double g_cam_est[3] = {0, 0, 0};
static float g_lvp[16];
static float g_gen_Ly = 1.0f, g_gen_Lz = 0.0f, g_ox0 = 0, g_oy0 = 0;   // dasar cahaya saat peta dibuat (dipakai shader)
static float g_bs[3] = {0, 0, 1}, g_bu[3] = {1, 0, 0}, g_bf[3] = {0, -1, 0};
static bool g_lvp_valid = false;
static float g_sun[3] = {0, 1, 0};
static bool g_sun_valid = false;
static unsigned g_sun_frame = 0, g_sun_seen_frame = 0;
static int g_sun_prog = 0, g_sun_learn = 0;
struct TexCls { int tex; int half; };
static TexCls g_tcls[8];
static int g_ntc = 0;
static unsigned g_st_frames = 0, g_st_delta_ok = 0, g_st_pass = 0, g_st_draws = 0, g_st_skipped = 0, g_st_logs = 0;
static double g_st_ms = 0;
static unsigned g_st_ent = 0;

static bool sun_enabled() {
  return g_sun_valid && g_sun[1] > 0.02f && (g_frames - g_sun_frame) < 3000;
}

// membaca posisi vertex pertama kotak (untuk mengenali kotak matahari/bulan); hasil: setengah sisi, 0 bila bukan kotak datar
static int read_quad_half(unsigned prog) {
  if (!f_getaloc || !f_vaiv || !f_vaptr || !f_geti) return 0;
  int loc = f_getaloc(prog, "POSITION");
  if (loc < 0) return 0;
  int en = 0, sz = 0, ty = 0, st = 0, bb = 0; void* po = nullptr;
  f_vaiv(loc, 0x8622, &en); f_vaiv(loc, 0x8623, &sz); f_vaiv(loc, 0x8625, &ty);
  f_vaiv(loc, 0x8624, &st); f_vaiv(loc, 0x889F, &bb); f_vaptr(loc, 0x8645, &po);
  if (!en || ty != 0x1406 || sz != 3) return 0;
  int stride = st ? st : 12;
  float v[4][3] = {{0}};
  if (bb == 0 && po) {
    for (int i = 0; i < 4; i++) memcpy(v[i], (const uint8_t*)po + i * stride, 12);
  } else if (bb != 0 && f_maprange && f_unmap) {
    int cur = 0; f_geti(0x8894, &cur);
    if (cur != bb) return 0;
    void* m = f_maprange(0x8892, (intptr_t)po, (intptr_t)(3 * stride + 12), 1);
    if (!m) return 0;
    for (int i = 0; i < 4; i++) memcpy(v[i], (const uint8_t*)m + i * stride, 12);
    f_unmap(0x8892);
  } else return 0;
  float h = v[2][0];
  if (fabsf(v[0][1]) > 0.01f || fabsf(v[2][1]) > 0.01f) return 0;
  if (fabsf(v[0][0] + v[2][0]) > 0.01f || fabsf(v[0][0] - v[0][2]) > 0.01f) return 0;
  if (h < 10.0f || h > 60.0f) return 0;
  return (int)(h + 0.5f);
}

// kotak kecil di luar terrain, digambar sesudah terrain: kandidat matahari/bulan
static void sky_draw(ProgStat* s, unsigned mode, int count) {
  if (mode != 4 || !g_view_ok || s->loc_wvp < 0 || count != 6) return;
  if ((int)s->id != g_sun_prog) {
    if (g_sun_prog != 0 || g_sun_learn > 80) return;
    unsigned char dm = 1; f_getb(0x0B72, &dm);
    if (dm || !f_isen(0x0BE2)) return;
    g_sun_learn++;
    if (read_quad_half(s->id) == 0) return;
    g_sun_prog = (int)s->id;
    sh_log("shadow: program matahari/bulan = %u\n", s->id);
  }
  int tex = 0; f_geti(0x8069, &tex);
  int half = 0;
  for (int i = 0; i < g_ntc; i++) if (g_tcls[i].tex == tex) half = g_tcls[i].half;
  if (!half) {
    half = read_quad_half(s->id);
    if (!half) return;
    if (g_ntc < 8) { g_tcls[g_ntc].tex = tex; g_tcls[g_ntc].half = half; g_ntc++; }
    else { g_tcls[g_frames & 7].tex = tex; g_tcls[g_frames & 7].half = half; }
  }
  float w[16];
  f_unifv(s->id, s->loc_wvp, w);
  float xv = w[12] / g_proj[0], yv = w[13] / g_proj[5], zv = -w[15];
  float len = sqrtf(xv * xv + yv * yv + zv * zv);
  if (len < 1e-6f) return;
  xv /= len; yv /= len; zv /= len;
  float d[3] = {g_wv[0] * xv + g_wv[1] * yv + g_wv[2] * zv,
                g_wv[4] * xv + g_wv[5] * yv + g_wv[6] * zv,
                g_wv[8] * xv + g_wv[9] * yv + g_wv[10] * zv};
  bool is_sun = half >= 25;   // matahari 60x60, bulan 40x40
  if (!is_sun) { d[0] = -d[0]; d[1] = -d[1]; d[2] = -d[2]; }
  if (is_sun || g_sun_seen_frame != g_frames) {
    memcpy(g_sun, d, sizeof(d));
    static bool first_sun = true;
    if (first_sun) { first_sun = false; bc("arah matahari pertama terdeteksi"); }
    g_sun_valid = true; g_sun_frame = g_frames;
    if (is_sun) g_sun_seen_frame = g_frames;
  }
}

static void water_frame_begin(ProgStat* s);    // bagian AIR: PANTULAN (di bawah)
static void water_capture_prog(ProgStat* s);
// dipanggil sebelum tiap draw terrain
static int g_u2_prev = 0, g_act_prev = 0;
// hasil true: slot tekstur 2 ditimpa dengan peta bayangan, panggil terrain_draw_post sesudah draw asli
static bool terrain_draw(ProgStat* s, int count, unsigned type, const void* idx) {
  if (!g_level_inited) level_init();
  if (g_level < 1 || g_cur_fbo != 0) return false;
  if (g_tframe != g_frames) {   // draw terrain pertama di frame ini
    g_tframe = g_frames;
    g_view_ok = false; g_en = 0;
    g_delta_exact = false;
    if (g_have_prev_delta) {   // belum ada chunk yang cocok: pakai dugaan (kamera bergerak sama seperti frame lalu), jangan matikan bayangan
      g_delta[0] = g_last_delta[0]; g_delta[1] = g_last_delta[1]; g_delta[2] = g_last_delta[2];
      g_delta_ok = true; g_delta_ver++;
    } else g_delta_ok = false;
    if (!g_guard_done) guard_arm();
    water_frame_begin(s);
    if (g_level >= 2 && g_sh_state == 1) {
      int act = 0x84C0; f_geti(0x84E0, &act);
      gx.ActiveTexture(0x84C0); f_geti(0x8069, &g_atlas_tex);
      gx.ActiveTexture((unsigned)act);
    }
  }
  if (!g_view_ok && f_unifv && s->loc_wv >= 0 && s->loc_proj >= 0) {
    f_unifv(s->id, s->loc_wv, g_wv); f_unifv(s->id, s->loc_proj, g_proj);
    g_view_ok = true;
  }
  if (g_level < 2 || g_sh_state != 1) return false;
  int vbo = 0, ibo = 0; f_geti(0x8894, &vbo); f_geti(0x8895, &ibo);
  float o[4] = {0, 0, 0, 1};
  if (s->loc_origin >= 0) f_unifv(s->id, s->loc_origin, o);
  bool blend = f_isen(0x0BE2) != 0;
  if (!blend) {
    int prev = 1 - g_dcur;
    if (!g_delta_exact && g_dn[prev] > 0) {   // chunk yang sama di frame sebelumnya -> selisih posisi kamera yang tepat
      for (int i = 0; i < g_dn[prev]; i++) {
        const DrawRec& r = g_dl[prev][i];
        if (r.vbo == vbo && r.ibo == ibo && r.count == count && r.idx == (uintptr_t)idx) {
          g_delta[0] = r.o[0] - o[0]; g_delta[1] = r.o[1] - o[1]; g_delta[2] = r.o[2] - o[2];
          g_last_delta[0] = g_delta[0]; g_last_delta[1] = g_delta[1]; g_last_delta[2] = g_delta[2]; g_have_prev_delta = true;
          g_cam_est[0] += g_delta[0]; g_cam_est[1] += g_delta[1]; g_cam_est[2] += g_delta[2];
          g_delta_ok = true; g_delta_exact = true; g_delta_ver++;
          break;
        }
      }
    }
    if (g_dn[g_dcur] < 2048 && count > 0 && vbo > 0 && ibo > 0) {
      int vsz = 0, isz = 0;
      gx.GetBufferParameteriv(0x8892, 0x8764, &vsz);   // GL_BUFFER_SIZE
      gx.GetBufferParameteriv(0x8893, 0x8764, &isz);
      water_capture_prog(s);
      DrawRec& r = g_dl[g_dcur][g_dn[g_dcur]++];
      r.vbo = vbo; r.ibo = ibo; r.count = count; r.vsz = vsz; r.isz = isz; r.type = type; r.idx = (uintptr_t)idx;
      r.o[0] = o[0]; r.o[1] = o[1]; r.o[2] = o[2]; r.pid = s->id; r.sc = o[3];
    }
  }
  if (g_level < 3) return false;
  bool want = g_lvp_valid && g_delta_ok && sun_enabled();
  if (!want) {
    if (s->vp_state == 1 && s->loc_vp >= 0) { gx.Uniform3f(s->loc_vp, s->vp_orig[0], s->vp_orig[1], s->vp_orig[2]); s->vp_state = 0; }
    return false;
  }
  if (s->loc_vp < 0 || s->loc_cc < 0 || s->loc_t2 < 0) return false;   // shader belum memuat jalur bayangan
  static bool first_set = true;
  if (first_set) { first_set = false; bc("parameter bayangan pertama diisi lewat VIEW_DIRECTION dan CURRENT_COLOR"); }
  if (s->vp_state == 0) f_unifv(s->id, s->loc_vp, s->vp_orig);   // simpan nilai asli supaya bisa dipulihkan
  // pergeseran kisi: peta dibuat di koordinat kamera frame sebelumnya, jadi tambahkan selisih kamera (g_delta)
  float ox = g_ox0 + g_bs[0] * g_delta[0] + g_bs[1] * g_delta[1] + g_bs[2] * g_delta[2];
  float oy = g_oy0 + g_bu[0] * g_delta[0] + g_bu[1] * g_delta[1] + g_bu[2] * g_delta[2];
  float oz = g_bf[0] * g_delta[0] + g_bf[1] * g_delta[1] + g_bf[2] * g_delta[2];
  float cc[4] = {0, 0, 0, 1};
  f_unifv(s->id, s->loc_cc, cc);                       // .r milik game (efek fade chunk) tetap dipertahankan
  gx.Uniform4f(s->loc_cc, cc[0], ox, oy, cc[3]);
  gx.Uniform3f(s->loc_vp, -(g_gen_Ly + 2.0f), g_gen_Lz, oz);   // VIEW_DIRECTION = (-(Ly+2), Lz, oz)
  s->vp_state = 1;
  gx.Uniform1i(s->loc_t2, 2);
  f_geti(0x84E0, &g_act_prev);
  gx.ActiveTexture(0x84C2); f_geti(0x8069, &g_u2_prev); gx.BindTexture(0x0DE1, g_sh_tex);
  gx.ActiveTexture((unsigned)g_act_prev);
  return true;
}
static void terrain_draw_post() {
  gx.ActiveTexture(0x84C2); gx.BindTexture(0x0DE1, (unsigned)g_u2_prev);
  gx.ActiveTexture((unsigned)g_act_prev);
}


// ---- bayangan mob dan item: rekam draw entity (program dengan uniform WORLD) ----
struct EntRec { int vbo, ibo, vsz, isz, count, first, psize, ptype, pnorm, pstride; unsigned mode, type; uintptr_t idx, poff; bool indexed; float m[16]; };
static EntRec g_el[512];
static unsigned g_ent_total = 0, g_ent_skip = 0, g_ent_hand = 0, g_ent_rec = 0;
static unsigned g_hand_frame = 0xFFFFFFFFu;   // frame terakhir yang punya tangan/item di layar (tanda orang pertama)

static void entity_draw(ProgStat* s, unsigned mode, int first, int count, unsigned type, const void* idx, bool indexed) {
  if (g_level < 4 || g_sh_state != 1 || g_cur_fbo != 0 || g_tframe != g_frames || count <= 0) return;
  g_ent_total++;
  if (s->loc_world < 0 || !f_unifv || !f_getaloc || !f_vaiv || !f_vaptr) return;
  float m[16];
  f_unifv(s->id, s->loc_world, m);
  float dist = sqrtf(m[12] * m[12] + m[13] * m[13] + m[14] * m[14]);
  if (dist < 1.2f) { g_hand_frame = g_frames; g_ent_hand++; return; }   // tangan / item yang dipegang
  if (dist > 90.0f || g_en >= 512) return;
  int loc = f_getaloc(s->id, "POSITION");
  if (loc < 0) { g_ent_skip++; return; }
  int en = 0, sz = 0, ty = 0, nm = 0, st = 0, bb = 0; void* po = nullptr;
  f_vaiv(loc, 0x8622, &en); f_vaiv(loc, 0x8623, &sz); f_vaiv(loc, 0x8625, &ty);
  f_vaiv(loc, 0x886A, &nm); f_vaiv(loc, 0x8624, &st); f_vaiv(loc, 0x889F, &bb); f_vaptr(loc, 0x8645, &po);
  int vbo = 0, ibo = 0; f_geti(0x8894, &vbo); f_geti(0x8895, &ibo);
  if (!en || bb == 0 || vbo != bb || sz < 2 || sz > 4 || (indexed && ibo == 0)) { g_ent_skip++; return; }   // array di memori biasa: tidak bisa diulang nanti
  int vsz = 0, isz = 0;
  gx.GetBufferParameteriv(0x8892, 0x8764, &vsz);
  if (indexed) gx.GetBufferParameteriv(0x8893, 0x8764, &isz);
  EntRec& r = g_el[g_en++];
  r.vbo = vbo; r.ibo = ibo; r.vsz = vsz; r.isz = isz; r.count = count; r.first = first; r.psize = sz; r.ptype = ty; r.pnorm = nm; r.pstride = st;
  r.mode = mode; r.type = type; r.idx = (uintptr_t)idx; r.poff = (uintptr_t)po; r.indexed = indexed;
  memcpy(r.m, m, sizeof(m));
  g_ent_rec++;
  static int first_logs = 0;
  if (first_logs < 6) {
    first_logs++;
    sh_log("mob: prog=%u jarak=%.1f posisi=(%.1f %.1f %.1f) pos attr size=%d type=0x%x stride=%d count=%d indexed=%d\n",
           s->id, dist, m[12], m[13], m[14], sz, ty, st, count, (int)indexed);
  }
}

// ---- simpan / pulihkan status GL ----
struct SavedState {
  int fbo, prog, arr, ele, act, tex0, vp[4], dfunc, cullmode;
  unsigned char dmask, cmask[4], depth, cull, blend, scissor, poff, stencil;
  float cleard, pofF, pofU;
  struct VA { int en, size, type, norm, stride, buf; void* ptr; } va[8];
};
static void save_state(SavedState* s) {
  f_geti(0x8CA6, &s->fbo); f_geti(0x8B8D, &s->prog); f_geti(0x8894, &s->arr); f_geti(0x8895, &s->ele);
  f_geti(0x84E0, &s->act); f_geti(0x0BA2, s->vp); f_geti(0x0B74, &s->dfunc); f_geti(0x0B45, &s->cullmode);
  f_getb(0x0B72, &s->dmask); f_getb(0x0C23, s->cmask);
  s->depth = f_isen(0x0B71); s->cull = f_isen(0x0B44); s->blend = f_isen(0x0BE2);
  s->scissor = f_isen(0x0C11); s->poff = f_isen(0x8037); s->stencil = f_isen(0x0B90);
  gx.GetFloatv(0x0B73, &s->cleard); gx.GetFloatv(0x8038, &s->pofF); gx.GetFloatv(0x2A00, &s->pofU);
  gx.ActiveTexture(0x84C0); f_geti(0x8069, &s->tex0); gx.ActiveTexture((unsigned)s->act);
  for (int i = 0; i < 8; i++) {
    SavedState::VA& v = s->va[i];
    v.en = v.size = v.type = v.norm = v.stride = v.buf = 0; v.ptr = nullptr;
    f_vaiv(i, 0x8622, &v.en); f_vaiv(i, 0x8623, &v.size); f_vaiv(i, 0x8625, &v.type);
    f_vaiv(i, 0x886A, &v.norm); f_vaiv(i, 0x8624, &v.stride); f_vaiv(i, 0x889F, &v.buf);
    f_vaptr(i, 0x8645, &v.ptr);
  }
}
static void set_cap(unsigned cap, unsigned char on) { if (on) gx.Enable(cap); else gx.Disable(cap); }
static void restore_state(const SavedState* s) {
  gx.BindFramebuffer(0x8D40, (unsigned)s->fbo);
  gx.Viewport(s->vp[0], s->vp[1], s->vp[2], s->vp[3]);
  gx.UseProgram((unsigned)s->prog);
  for (int i = 0; i < 8; i++) {
    const SavedState::VA& v = s->va[i];
    if ((v.en || v.buf) && v.size >= 1 && v.size <= 4) {
      gx.BindBuffer(0x8892, (unsigned)v.buf);
      gx.VertexAttribPointer((unsigned)i, v.size, (unsigned)v.type, (unsigned char)v.norm, v.stride, v.ptr);
    }
    if (v.en) gx.EnableVertexAttribArray((unsigned)i); else gx.DisableVertexAttribArray((unsigned)i);
  }
  gx.BindBuffer(0x8892, (unsigned)s->arr);
  gx.BindBuffer(0x8893, (unsigned)s->ele);
  gx.ActiveTexture(0x84C0); gx.BindTexture(0x0DE1, (unsigned)s->tex0);
  gx.ActiveTexture((unsigned)s->act);
  set_cap(0x0B71, s->depth); set_cap(0x0B44, s->cull); set_cap(0x0BE2, s->blend);
  set_cap(0x0C11, s->scissor); set_cap(0x8037, s->poff); set_cap(0x0B90, s->stencil);
  gx.DepthMask(s->dmask); gx.ColorMask(s->cmask[0], s->cmask[1], s->cmask[2], s->cmask[3]);
  gx.DepthFunc((unsigned)s->dfunc); gx.CullFace((unsigned)s->cullmode);
  gx.PolygonOffset(s->pofF, s->pofU); gx.ClearDepthf(s->cleard);
}

// matriks 4x4 kolom-utama: C = A * B
static void mat_mul(const float* A, const float* B, float* C) {
  for (int c = 0; c < 4; c++)
    for (int r = 0; r < 4; r++) {
      float v = 0;
      for (int k = 0; k < 4; k++) v += A[k * 4 + r] * B[c * 4 + k];
      C[c * 4 + r] = v;
    }
}

static void build_light_matrix(float* lvp) {
  const double R = (double)g_cfg_radius;
  const float D = SH_DEPTH_D;
  float Lv[3] = {0.0f, g_sun[1], g_sun[2]};   // matahari selalu bergerak di bidang Y-Z, jadi x dipaksa 0
  float ln = sqrtf(Lv[0] * Lv[0] + Lv[1] * Lv[1] + Lv[2] * Lv[2]);
  Lv[0] /= ln; Lv[1] /= ln; Lv[2] /= ln;
  float f[3] = {-Lv[0], -Lv[1], -Lv[2]};
  float up[3] = {1, 0, 0};
  if (fabsf(Lv[0]) > 0.95f) { up[0] = 0; up[1] = 1; }
  // s = normalize(cross(f, up)), u = cross(s, f)
  float s[3] = {f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2], f[0] * up[1] - f[1] * up[0]};
  float sn = sqrtf(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
  s[0] /= sn; s[1] /= sn; s[2] /= sn;
  float u[3] = {s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0]};
  // pusat peta dikunci ke kisi texel di dunia supaya tepi bayangan tidak bergeser saat pemain bergerak
  double texel = 2.0 * R / (double)g_sh_size;
  double a = g_cam_est[0] * s[0] + g_cam_est[1] * s[1] + g_cam_est[2] * s[2];
  double b = g_cam_est[0] * u[0] + g_cam_est[1] * u[1] + g_cam_est[2] * u[2];
  double sa = floor(a / texel + 0.5) * texel, sb = floor(b / texel + 0.5) * texel;
  float c[3];
  for (int i = 0; i < 3; i++) c[i] = (float)((sa - a) * s[i] + (sb - b) * u[i]);
  g_gen_Ly = Lv[1]; g_gen_Lz = Lv[2];
  g_ox0 = (float)(a - sa); g_oy0 = (float)(b - sb);   // -(s.c) dan -(u.c)
  for (int i = 0; i < 3; i++) { g_bs[i] = s[i]; g_bu[i] = u[i]; g_bf[i] = f[i]; }
  float eye[3] = {c[0] + Lv[0] * D, c[1] + Lv[1] * D, c[2] + Lv[2] * D};
  float V[16] = {s[0], u[0], -f[0], 0, s[1], u[1], -f[1], 0, s[2], u[2], -f[2], 0,
                 -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]),
                 -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]),
                 (f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2]), 1};
  float nr = 1.0f, fr = 2.0f * D, Rf = (float)R;
  float P[16] = {1.0f / Rf, 0, 0, 0, 0, 1.0f / Rf, 0, 0, 0, 0, -2.0f / (fr - nr), 0,
                 0, 0, -(fr + nr) / (fr - nr), 1};
  mat_mul(P, V, lvp);
}

// dijalankan tepat sebelum eglSwapBuffers; menggambar ulang terrain frame ini dari arah matahari
static void shadow_pass() {
  g_lvp_valid = false;
  int cur = g_dcur, n = g_dn[cur];
  if (n == 0 || !sun_enabled()) return;
  static bool first_pass = true;
  if (first_pass) bc("pass bayangan pertama mulai");
  float lvp[16];
  build_light_matrix(lvp);
  double t0 = mono_sec();
  SavedState st;
  save_state(&st);
  gx.BindFramebuffer(0x8D40, g_sh_fbo);
  gx.Viewport(0, 0, g_sh_size, g_sh_size);
  gx.Disable(0x0C11); gx.Disable(0x0B90); gx.Disable(0x0BE2); gx.Disable(0x0B44);
  gx.Enable(0x0B71); gx.DepthFunc(0x0201); gx.DepthMask(1); gx.ColorMask(0, 0, 0, 0);
  gx.Enable(0x8037); gx.PolygonOffset(1.5f, 2.0f);
  gx.ClearDepthf(1.0f); gx.Clear(0x100);
  gx.UseProgram(g_sh_prog);
  gx.UniformMatrix4fv(g_sh_loc_lvp, 1, 0, lvp);
  gx.Uniform1i(g_sh_loc_tex, 0);
  gx.ActiveTexture(0x84C0); gx.BindTexture(0x0DE1, (unsigned)g_atlas_tex);
  for (int i = 0; i < 4; i++) gx.DisableVertexAttribArray((unsigned)i);
  gx.EnableVertexAttribArray(0); gx.EnableVertexAttribArray(1);
  float margin = 14.0f / g_cfg_radius;
  unsigned drawn = 0, skipped = 0;
  for (int i = 0; i < n && drawn < 1800; i++) {
    const DrawRec& r = g_dl[cur][i];
    float cx = r.o[0] + 8.0f, cy = r.o[1] + 8.0f, cz = r.o[2] + 8.0f;
    float x = lvp[0] * cx + lvp[4] * cy + lvp[8] * cz + lvp[12];
    float y = lvp[1] * cx + lvp[5] * cy + lvp[9] * cz + lvp[13];
    if (fabsf(x) > 1.0f + margin || fabsf(y) > 1.0f + margin) { skipped++; continue; }
    gx.BindBuffer(0x8892, (unsigned)r.vbo);
    gx.BindBuffer(0x8893, (unsigned)r.ibo);
    int vs = 0, is = 0;
    gx.GetBufferParameteriv(0x8892, 0x8764, &vs);
    gx.GetBufferParameteriv(0x8893, 0x8764, &is);
    if (vs != r.vsz || is != r.isz || vs <= 0 || is <= 0) { skipped++; continue; }   // buffer sudah diganti/dihapus game
    gx.VertexAttribPointer(0, 3, 0x1406, 0, 24, (const void*)0);
    gx.VertexAttribPointer(1, 2, 0x1403, 1, 24, (const void*)20);
    gx.Uniform3f(g_sh_loc_org, r.o[0], r.o[1], r.o[2]);
    gx.DrawElements(4, r.count, r.type, (const void*)r.idx);
    drawn++;
  }
  // ---- mob, item, dan kotak pemain (orang pertama) ----
  unsigned edrawn = 0;
  if (g_level >= 4 && g_eh_prog && g_eh_loc >= 0) {
    gx.UseProgram(g_eh_prog);
    gx.DisableVertexAttribArray(1);
    gx.EnableVertexAttribArray(0);
    float emg = 6.0f / g_cfg_radius;
    for (int i = 0; i < g_en; i++) {
      const EntRec& r = g_el[i];
      float x = lvp[0] * r.m[12] + lvp[4] * r.m[13] + lvp[8] * r.m[14] + lvp[12];
      float y = lvp[1] * r.m[12] + lvp[5] * r.m[13] + lvp[9] * r.m[14] + lvp[13];
      if (fabsf(x) > 1.0f + emg || fabsf(y) > 1.0f + emg) continue;
      gx.BindBuffer(0x8892, (unsigned)r.vbo);
      int evs = 0, eis = 0;
      gx.GetBufferParameteriv(0x8892, 0x8764, &evs);
      if (evs != r.vsz || evs <= 0) continue;
      if (r.indexed) {
        gx.BindBuffer(0x8893, (unsigned)r.ibo);
        gx.GetBufferParameteriv(0x8893, 0x8764, &eis);
        if (eis != r.isz || eis <= 0) continue;
      }
      gx.VertexAttribPointer(0, r.psize, (unsigned)r.ptype, (unsigned char)r.pnorm, r.pstride, (const void*)r.poff);
      float lm[16]; mat_mul(lvp, r.m, lm);
      gx.UniformMatrix4fv(g_eh_loc, 1, 0, lm);
      if (r.indexed) gx.DrawElements(r.mode, r.count, r.type, (const void*)r.idx);
      else gx.DrawArrays(r.mode, (int)r.first, r.count);
      edrawn++;
    }
    // game tidak menggambar tubuh pemain di orang pertama: dipasang kotak sederhana (0.6 x 1.8 x 0.4) di posisi kamera
    if (g_cfg_playerbox && g_cube_vbo && g_cube_ibo && g_hand_frame + 1 == g_frames && g_view_ok) {
      float fx = -g_wv[2], fz = -g_wv[10];
      float fl = sqrtf(fx * fx + fz * fz);
      if (fl > 1e-4f) {
        fx /= fl; fz /= fl;
        float M[16] = {fz * 0.6f, 0, -fx * 0.6f, 0,   0, 1.8f, 0, 0,   fx * 0.4f, 0, fz * 0.4f, 0,   0, -0.72f, 0, 1};
        float lm[16]; mat_mul(lvp, M, lm);
        gx.BindBuffer(0x8892, g_cube_vbo); gx.BindBuffer(0x8893, g_cube_ibo);
        gx.VertexAttribPointer(0, 3, 0x1406, 0, 12, (const void*)0);
        gx.UniformMatrix4fv(g_eh_loc, 1, 0, lm);
        gx.DrawElements(4, 36, 0x1403, (const void*)0);
        edrawn++;
      }
    }
  }
  g_st_ent += edrawn;
  restore_state(&st);
  unsigned err = gx.GetError();
  clear_gl_errors();
  memcpy(g_lvp, lvp, sizeof(lvp));
  g_lvp_valid = true;
  if (first_pass) { first_pass = false; bc("pass bayangan pertama selesai"); }
  g_st_pass++; g_st_draws += drawn; g_st_skipped += skipped; g_st_ms += (mono_sec() - t0) * 1000.0;
  if (err != 0 && g_st_logs < 8) { g_st_logs++; sh_log("shadow: kesalahan GL 0x%x setelah pass (frame %u)\n", err, g_frames); }
}

// akhir frame: gambar peta bayangan lalu tukar daftar draw
static void shadow_end_frame() {
  if (g_level < 1) return;
  bool had_terrain = (g_tframe + 1 == g_frames);   // g_frames sudah naik di awal spy_swap
  if (had_terrain) {
    g_guard_frames++;
    if (g_guard_armed && g_guard_frames >= 90) {
      guard_write(g_level, 0); g_guard_armed = false;
      sh_log("pipe3d: level %d lolos 90 frame terrain tanpa crash\n", g_level);
    }
  }
  if (g_level < 2 || !had_terrain) return;
  if (g_sh_state == 0) {   // inisialisasi di titik aman: akhir frame, tidak sedang di tengah draw game
    bc("shadow_init mulai");
    shadow_init();
    bc(g_sh_state == 1 ? "shadow_init selesai (siap)" : "shadow_init selesai (gagal/nonaktif)");
    return;
  }
  if (g_sh_state != 1) return;
  g_st_frames++;
  if (g_delta_exact) g_st_delta_ok++;
  else if (g_delta_ok) { g_cam_est[0] += g_delta[0]; g_cam_est[1] += g_delta[1]; g_cam_est[2] += g_delta[2]; }   // tidak ada chunk cocok: pakai dugaan
  shadow_pass();
  g_dcur = 1 - g_dcur;
  g_dn[g_dcur] = 0;
  if (g_frames % 600 == 0 && g_st_logs < 12) {
    g_st_logs++;
    sh_log("shadow frame %u: pass=%u draw/pass=%.0f lewati/pass=%.0f ms/pass=%.2f delta_tepat=%u/%u matahari=(%.3f %.3f %.3f) valid=%d lvp=%d | mob: total=%u terekam=%u dilewati=%u tangan=%u digambar/pass=%.1f orang_pertama=%d\n",
           g_frames, g_st_pass, g_st_pass ? (double)g_st_draws / g_st_pass : 0.0, g_st_pass ? (double)g_st_skipped / g_st_pass : 0.0,
           g_st_pass ? g_st_ms / g_st_pass : 0.0, g_st_delta_ok, g_st_frames, g_sun[0], g_sun[1], g_sun[2],
           (int)g_sun_valid, (int)g_lvp_valid, g_ent_total, g_ent_rec, g_ent_skip, g_ent_hand, g_st_pass ? (double)g_st_ent / g_st_pass : 0.0,
           (int)(g_hand_frame + 1 == g_frames));
    g_st_pass = g_st_draws = g_st_skipped = g_st_delta_ok = g_st_frames = 0; g_st_ms = 0; g_st_ent = 0;
    g_ent_total = g_ent_rec = g_ent_skip = g_ent_hand = 0;
  }
}

// ===================== AIR: PANTULAN (bidang pantul, tiru mod water shader Java) =====================
// Cara kerja (semua dari libpipe3d, shader game TIDAK diubah, tidak ada sampler/uniform baru di shader game):
//  1. Draw terrain (opaque + alpha test) tiap frame sudah direkam oleh bagian peta bayangan (g_dl).
//  2. Tepat sebelum draw blend pertama (air) di frame itu, terrain digambar ulang TERBALIK terhadap bidang air
//     ke tekstur warna kecil (setengah resolusi). Memakai program terrain game sendiri, jadi warna/cahaya/kabut sama.
//     Bidang potong miring (oblique near plane) membuang semua yang ada di bawah permukaan air.
//  3. Sesudah tiap draw air bawaan, geometri air yang sama digambar sekali lagi dengan program kecil milik pipe3d:
//     hanya fragmen di ketinggian permukaan air yang dipakai, warnanya = pantulan (dibaca per piksel layar, digeser riak
//     gelombang sinus seperti water.frag di mod Java), alpha = fresnel (makin miring makin memantul).
//  Pengaturan opsional di /sdcard/games/com.mojang/pipe3d_water.txt: on=1 stage=2 scale=0.5 radius=72 watery=62.889 strength=1.0 lift=0.02 tol=0.07
//  stage 0 mati; 1 hanya menggambar tekstur pantulan (tidak terlihat, untuk uji aman); 2 tampil di air.
//  Pengaman crash sendiri (pipe3d_state_w.txt): kalau peluncuran sebelumnya tidak lolos 120 frame pantulan, stage turun satu.
//  Butuh: bagian bayangan level >= 2 (daftar draw), dan hook Level::animateTick (posisi pemain) untuk ketinggian kamera.
static int g_w_stage = -1;   // -1 belum dibaca
static float g_w_scale = 0.5f, g_w_rr = 72.0f, g_w_water_y = 62.889f, g_w_str = 1.0f, g_w_lift = 0.02f, g_w_tol = 0.07f;
static const char* WGUARD_FILE = "/sdcard/games/com.mojang/pipe3d_state_w.txt";
static bool g_w_armed = false;
static unsigned g_w_okframes = 0, g_w_logs = 0;
static double g_w_ms = 0; static unsigned g_w_passes = 0, g_w_draws = 0;
// posisi pemain dari hook Level::animateTick
static float g_pl_x = 0, g_pl_y = 0, g_pl_z = 0;
static bool g_pl_ok = false;
static unsigned g_pl_frame = 0;
static float g_w_camy = 0;

static void wguard_write(int st, int armed) {
  FILE* f = fopen(WGUARD_FILE, "w");
  if (f) { fprintf(f, "stage=%d\narmed=%d\n", st, armed); fclose(f); }
}
static void water_cfg_init() {
  cfg_load();
  int on = 1, forced = -1, stage = 2, armed = 0; bool have = false;
  FILE* f = fopen("/sdcard/games/com.mojang/pipe3d_water.txt", "r");
  if (f) {
    char line[128];
    while (fgets(line, sizeof(line), f)) {
      int iv; float fv;
      if (sscanf(line, "on=%d", &iv) == 1) on = iv;
      else if (sscanf(line, "stage=%d", &iv) == 1) forced = iv;
      else if (sscanf(line, "scale=%f", &fv) == 1) g_w_scale = (fv < 0.25f) ? 0.25f : (fv > 1.0f ? 1.0f : fv);
      else if (sscanf(line, "radius=%f", &fv) == 1) g_w_rr = (fv < 24.0f) ? 24.0f : (fv > 160.0f ? 160.0f : fv);
      else if (sscanf(line, "watery=%f", &fv) == 1) g_w_water_y = fv;
      else if (sscanf(line, "strength=%f", &fv) == 1) g_w_str = (fv < 0.0f) ? 0.0f : (fv > 1.5f ? 1.5f : fv);
      else if (sscanf(line, "lift=%f", &fv) == 1) g_w_lift = (fv < 0.0f) ? 0.0f : (fv > 0.2f ? 0.2f : fv);
      else if (sscanf(line, "tol=%f", &fv) == 1) g_w_tol = (fv < 0.01f) ? 0.01f : (fv > 0.3f ? 0.3f : fv);
    }
    fclose(f);
  }
  f = fopen(WGUARD_FILE, "r");
  if (f) {
    char line[64]; int v;
    while (fgets(line, sizeof(line), f)) {
      if (sscanf(line, "stage=%d", &v) == 1) { stage = v; have = true; }
      else if (sscanf(line, "armed=%d", &v) == 1) armed = v;
    }
    fclose(f);
  }
  if (forced >= 0) { stage = forced; armed = 0; }
  else if (have && armed) { sh_log("air: peluncuran sebelumnya tidak lolos 120 frame pantulan pada stage %d, turun ke stage %d\n", stage, stage > 0 ? stage - 1 : 0); stage = stage > 0 ? stage - 1 : 0; }
  if (!on) stage = 0;
  if (stage < 0) stage = 0;
  if (stage > 2) stage = 2;
  g_w_stage = stage;
  wguard_write(stage, 0);
  sh_log("air: stage=%d scale=%.2f radius=%.0f watery=%.3f strength=%.2f lift=%.3f tol=%.3f\n", stage, g_w_scale, g_w_rr, g_w_water_y, g_w_str, g_w_lift, g_w_tol);
}

// ---- matriks ----
static bool mat_inv4(const float* m, float* out) {
  float inv[16];
  inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
  inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
  inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
  inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
  inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
  inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
  inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
  inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
  inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
  inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
  inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
  inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
  inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
  inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
  inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
  inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
  float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
  if (fabsf(det) < 1e-12f) return false;
  det = 1.0f / det;
  for (int i = 0; i < 16; i++) out[i] = inv[i] * det;
  return true;
}
// Lengyel: bidang near proyeksi dimiringkan ke bidang C (ruang mata); sisi positif C dipertahankan, kamera di sisi negatif
static bool oblique_proj(float* p, const float* C) {
  float q[4];
  q[0] = (((C[0] > 0) - (C[0] < 0)) + p[8]) / p[0];
  q[1] = (((C[1] > 0) - (C[1] < 0)) + p[9]) / p[5];
  q[2] = -1.0f;
  q[3] = (1.0f + p[10]) / p[14];
  float d = C[0] * q[0] + C[1] * q[1] + C[2] * q[2] + C[3] * q[3];
  if (d < 0.02f) return false;   // bidang air ada di belakang kamera (kamera menengadah): air tidak terlihat, lewati
  float a = 2.0f / d;
  p[2] = C[0] * a; p[6] = C[1] * a; p[10] = C[2] * a + 1.0f; p[14] = C[3] * a;
  return true;
}

// ---- sumber daya ----
static unsigned g_w_tex = 0, g_w_fbo = 0, g_w_rb = 0, g_w_prog = 0;
static int g_w_tw = 0, g_w_th = 0, g_w_prog_pos = -2;
static int g_wu_wv = -1, g_wu_proj = -1, g_wu_org = -1, g_wu_lift = -1, g_wu_refl = -1, g_wu_vp = -1, g_wu_plane = -1,
           g_wu_time = -1, g_wu_str = -1, g_wu_tol = -1, g_wu_camxz = -1, g_wu_skyh = -1, g_wu_skyz = -1;
static bool g_w_ready = false;
static unsigned g_w_pass_frame = 0xFFFFFFFFu, g_w_uni_frame = 0xFFFFFFFFu;
static float g_w_h = 0;
static float g_w_fog[3] = {0.62f, 0.74f, 0.93f};

static const char* WTR_VS =
  "attribute vec3 POSITION;\n"
  "uniform mat4 WV;\n"
  "uniform mat4 PROJ;\n"
  "uniform vec4 ORG;\n"
  "uniform float LIFT;\n"
  "varying vec3 vp;\n"
  "void main() {\n"
  "  vec3 p = POSITION * ORG.w + ORG.xyz;\n"
  "  vp = p;\n"
  "  p.y += LIFT;\n"
  "  gl_Position = PROJ * (WV * vec4(p, 1.0));\n"
  "}\n";
static const char* WTR_FS =
  "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
  "precision highp float;\n"
  "#else\n"
  "precision mediump float;\n"
  "#endif\n"
  "varying vec3 vp;\n"
  "uniform sampler2D REFL;\n"
  "uniform vec4 VP;\n"
  "uniform float PLANE;\n"
  "uniform float TIME;\n"
  "uniform float STR;\n"
  "uniform float TOL;\n"
  "uniform vec2 CAMXZ;\n"
  "uniform vec3 SKYH;\n"
  "uniform vec3 SKYZ;\n"
  "void main() {\n"
  "  if (abs(vp.y - PLANE) > TOL) discard;\n"
  "  float dist = max(length(vp), 0.001);\n"
  "  vec3 v = vp / dist;\n"
  "  float cosT = clamp(abs(v.y), 0.0, 1.0);\n"
  "  vec2 wp = vp.xz + CAMXZ;\n"
  "  float t = TIME;\n"
  "  vec2 d = vec2(sin(wp.x * 1.3 + t * 1.1 + sin(wp.y * 0.7 + t * 0.6) * 1.5),\n"
  "                sin(wp.y * 1.1 - t * 0.9 + sin(wp.x * 0.8 - t * 0.5) * 1.5));\n"
  "  d += 0.5 * vec2(sin(wp.x * 3.1 + wp.y * 1.7 + t * 1.7), sin(wp.y * 3.3 - wp.x * 1.3 + t * 1.4));\n"
  "  vec2 uv = (gl_FragCoord.xy - VP.xy) / VP.zw;\n"
  "  uv += d * (0.007 / (1.0 + dist * 0.05));\n"
  "  uv = clamp(uv, vec2(0.003), vec2(0.997));\n"
  "  vec4 tx = texture2D(REFL, uv);\n"
  "  vec3 sky = mix(SKYH, SKYZ, smoothstep(0.0, 0.8, cosT));\n"
  "  vec3 refl = mix(sky, tx.rgb, clamp(tx.a * 1.5, 0.0, 1.0));\n"
  "  refl *= vec3(0.80, 0.95, 0.93);\n"
  "  float fr = pow(1.0 - cosT, 4.0);\n"
  "  float alpha = clamp((0.28 + 0.72 * fr) * STR, 0.0, 0.95);\n"
  "  gl_FragColor = vec4(refl, alpha);\n"
  "}\n";

static bool water_resources(int w, int h) {
  if (g_w_tex && w == g_w_tw && h == g_w_th) return true;
  clear_gl_errors();
  int prev_fbo = 0, prev_rb = 0, prev_tex = 0, prev_act = 0;
  f_geti(0x8CA6, &prev_fbo); f_geti(0x8CA7, &prev_rb); f_geti(0x84E0, &prev_act);
  gx.ActiveTexture(0x84C3); f_geti(0x8069, &prev_tex);
  bool first = (g_w_tex == 0);
  if (first) { gx.GenTextures(1, &g_w_tex); gx.GenRenderbuffers(1, &g_w_rb); gx.GenFramebuffers(1, &g_w_fbo); }
  gx.BindTexture(0x0DE1, g_w_tex);
  gx.TexImage2D(0x0DE1, 0, 0x1908, w, h, 0, 0x1908, 0x1401, nullptr);
  gx.TexParameteri(0x0DE1, 0x2801, 0x2601);   // MIN = LINEAR
  gx.TexParameteri(0x0DE1, 0x2800, 0x2601);   // MAG = LINEAR
  gx.TexParameteri(0x0DE1, 0x2802, 0x812F);
  gx.TexParameteri(0x0DE1, 0x2803, 0x812F);
  gx.BindRenderbuffer(0x8D41, g_w_rb);
  clear_gl_errors();
  gx.RenderbufferStorage(0x8D41, 0x81A6, w, h);   // DEPTH_COMPONENT24
  if (gx.GetError() != 0) gx.RenderbufferStorage(0x8D41, 0x81A5, w, h);   // DEPTH_COMPONENT16
  gx.BindFramebuffer(0x8D40, g_w_fbo);
  gx.FramebufferTexture2D(0x8D40, 0x8CE0, 0x0DE1, g_w_tex, 0);
  gx.FramebufferRenderbuffer(0x8D40, 0x8D00, 0x8D41, g_w_rb);
  unsigned status = gx.CheckFramebufferStatus(0x8D40);
  gx.BindFramebuffer(0x8D40, (unsigned)prev_fbo);
  gx.BindRenderbuffer(0x8D41, (unsigned)prev_rb);
  gx.BindTexture(0x0DE1, (unsigned)prev_tex);
  gx.ActiveTexture((unsigned)prev_act);
  clear_gl_errors();
  sh_log("air: framebuffer pantulan %dx%d status=0x%x (0x8cd5 = lengkap)\n", w, h, status);
  if (status != 0x8CD5) return false;
  g_w_tw = w; g_w_th = h;
  return true;
}

static bool water_make_prog(int posloc) {
  if (g_w_prog) { gx.DeleteProgram(g_w_prog); g_w_prog = 0; }
  unsigned vs = sh_compile(0x8B31, WTR_VS), fs = sh_compile(0x8B30, WTR_FS);
  if (!vs || !fs) return false;
  unsigned p = gx.CreateProgram();
  gx.AttachShader(p, vs); gx.AttachShader(p, fs);
  gx.BindAttribLocation(p, (unsigned)posloc, "POSITION");
  gx.LinkProgram(p);
  int linked = 0; gx.GetProgramiv(p, 0x8B82, &linked);
  if (!linked) { char buf[512] = {0}; int n = 0; gx.GetProgramInfoLog(p, 511, &n, buf); sh_log("air: link gagal: %s\n", buf); return false; }
  g_w_prog = p; g_w_prog_pos = posloc;
  g_wu_wv = f_getuloc(p, "WV"); g_wu_proj = f_getuloc(p, "PROJ"); g_wu_org = f_getuloc(p, "ORG"); g_wu_lift = f_getuloc(p, "LIFT");
  g_wu_refl = f_getuloc(p, "REFL"); g_wu_vp = f_getuloc(p, "VP"); g_wu_plane = f_getuloc(p, "PLANE"); g_wu_time = f_getuloc(p, "TIME");
  g_wu_str = f_getuloc(p, "STR"); g_wu_tol = f_getuloc(p, "TOL"); g_wu_camxz = f_getuloc(p, "CAMXZ");
  g_wu_skyh = f_getuloc(p, "SKYH"); g_wu_skyz = f_getuloc(p, "SKYZ");
  sh_log("air: program pantulan siap (POSITION di lokasi %d)\n", posloc);
  return true;
}

// ---- catatan per program terrain (tata letak atribut vertex, nilai uniform yang akan dipulihkan) ----
struct WLay { int loc, size, type, norm, stride; uintptr_t off; bool ok; };
struct WProg { unsigned id; unsigned frame; bool usable; WLay lay[4]; float wv[16], pr[16], org[4], vp[4]; };
static WProg g_wp[8];
static int g_nwp = 0;
static WProg* wprog_find(unsigned id) {
  for (int i = 0; i < g_nwp; i++) if (g_wp[i].id == id) return &g_wp[i];
  return nullptr;
}
// dipanggil di awal frame terrain (dari terrain_draw): baca pengaturan, catat warna kabut
static void water_frame_begin(ProgStat* s) {
  g_w_ready = false;
  if (g_w_stage < 0) {
    water_cfg_init();
    cap_init();
    if (!g_gx_ok) gx_init();
    if (!g_gx_ok || !f_getaloc || !f_getuloc || !f_unifv || !f_vaiv || !f_vaptr || !f_geti || !f_isen || !f_getb) { sh_log("air: fungsi GL tidak lengkap, dimatikan\n"); g_w_stage = 0; }
  }
  if (g_w_stage < 1) return;
  int fl = f_getuloc(s->id, "FOG_COLOR");
  if (fl >= 0) {
    float v[4] = {g_w_fog[0], g_w_fog[1], g_w_fog[2], 1.0f};
    f_unifv(s->id, fl, v);
    for (int i = 0; i < 3; i++) g_w_fog[i] = v[i] < 0 ? 0 : (v[i] > 1 ? 1 : v[i]);
  }
}
// dipanggil tiap draw terrain yang direkam (bukan blend)
static void water_capture_prog(ProgStat* s) {
  if (g_w_stage < 1) return;
  WProg* p = wprog_find(s->id);
  if (!p) {
    if (g_nwp >= 8) return;
    p = &g_wp[g_nwp++];
    memset(p, 0, sizeof(*p));
    p->id = s->id; p->frame = 0xFFFFFFFFu;
  }
  if (p->frame == g_frames) return;
  p->frame = g_frames;
  p->usable = (s->kind == 1 && s->loc_wv >= 0 && s->loc_proj >= 0 && s->loc_origin >= 0);
  static const char* an[4] = {"POSITION", "COLOR", "TEXCOORD_0", "TEXCOORD_1"};
  for (int i = 0; i < 4; i++) {
    WLay& L = p->lay[i];
    L.ok = false;
    int loc = f_getaloc(s->id, an[i]);
    L.loc = loc;
    if (loc < 0) continue;
    int en = 0, sz = 0, ty = 0, nm = 0, st = 0; void* po = nullptr;
    f_vaiv((unsigned)loc, 0x8622, &en);
    if (!en) continue;
    f_vaiv((unsigned)loc, 0x8623, &sz); f_vaiv((unsigned)loc, 0x8625, &ty);
    f_vaiv((unsigned)loc, 0x886A, &nm); f_vaiv((unsigned)loc, 0x8624, &st);
    f_vaptr((unsigned)loc, 0x8645, &po);
    if (sz < 1 || sz > 4) continue;
    L.size = sz; L.type = ty; L.norm = nm; L.stride = st; L.off = (uintptr_t)po; L.ok = true;
  }
  if (!p->lay[0].ok) p->usable = false;   // tanpa POSITION tidak bisa digambar
}

// ketinggian bidang air relatif kamera. Tinggi kamera = kelipatan 16 (asal chunk) dikurangi origin chunk, dipilih yang paling dekat dengan posisi pemain
static bool water_plane(float* h_out) {
  int cur = g_dcur;
  if (g_dn[cur] <= 0 || !g_pl_ok || g_frames - g_pl_frame > 900) return false;
  float oy = g_dl[cur][0].o[1];
  float k = floorf((g_pl_y + oy) / 16.0f + 0.5f);
  float camy = 16.0f * k - oy;
  g_w_camy = camy;
  *h_out = g_w_water_y - camy;
  return camy > g_w_water_y + 0.05f;   // kamera di atas permukaan
}

static bool water_reflect_pass() {
  double t0 = mono_sec();
  int cur = g_dcur, n = g_dn[cur];
  float h = g_w_h;
  float Mref[16] = {1, 0, 0, 0,  0, -1, 0, 0,  0, 0, 1, 0,  0, 2.0f * h, 0, 1};
  float wv2[16]; mat_mul(g_wv, Mref, wv2);
  float pr2[16]; memcpy(pr2, g_proj, sizeof(pr2));
  float inv[16];
  bool okc = false;
  if (mat_inv4(wv2, inv)) {
    const float Po[4] = {0, 1, 0, -h};   // sisi y_asli >= h dipertahankan
    float Pe[4];
    for (int i = 0; i < 4; i++) Pe[i] = inv[i * 4 + 0] * Po[0] + inv[i * 4 + 1] * Po[1] + inv[i * 4 + 2] * Po[2] + inv[i * 4 + 3] * Po[3];
    if (Pe[3] < -1e-4f && fabsf(pr2[0]) > 1e-6f && fabsf(pr2[5]) > 1e-6f && fabsf(pr2[14]) > 1e-6f) okc = oblique_proj(pr2, Pe);
  }
  if (!okc) return false;   // tanpa bidang potong miring bagian bawah air ikut tergambar; air juga tidak terlihat bila kamera menengadah
  SavedState st;
  save_state(&st);
  float clr[4] = {0, 0, 0, 0};
  gx.GetFloatv(0x0C22, clr);
  // simpan uniform semua program terrain yang dipakai frame ini
  for (int i = 0; i < g_nwp; i++) {
    WProg* p = &g_wp[i];
    if (p->frame != g_frames || !p->usable) continue;
    ProgStat* ps = prog_stat(p->id);
    f_unifv(p->id, ps->loc_wv, p->wv); f_unifv(p->id, ps->loc_proj, p->pr);
    p->org[0] = p->org[1] = p->org[2] = 0; p->org[3] = 1;
    f_unifv(p->id, ps->loc_origin, p->org);
    p->vp[0] = p->vp[1] = p->vp[2] = p->vp[3] = 0;
    if (ps->loc_vp >= 0) f_unifv(p->id, ps->loc_vp, p->vp);
  }
  gx.BindFramebuffer(0x8D40, g_w_fbo);
  gx.Viewport(0, 0, g_w_tw, g_w_th);
  gx.Disable(0x0C11); gx.Disable(0x0B90); gx.Disable(0x0BE2); gx.Disable(0x8037);
  gx.Enable(0x0B71); gx.DepthFunc(0x0203); gx.DepthMask(1); gx.ColorMask(1, 1, 1, 1);
  gx.ClearColor(0, 0, 0, 0); gx.ClearDepthf(1.0f); gx.Clear(0x4100);
  gx.FrontFace(0x0900);   // cermin membalik arah putar segitiga
  float rr2 = g_w_rr * g_w_rr;
  unsigned drawn = 0, skipped = 0;
  unsigned curpid = 0; ProgStat* cps = nullptr; WProg* cp = nullptr;
  for (int i = 0; i < n && drawn < 1400; i++) {
    const DrawRec& r = g_dl[cur][i];
    float dx = r.o[0] + 8.0f, dz = r.o[2] + 8.0f;
    if (dx * dx + dz * dz > rr2) { skipped++; continue; }
    if (r.o[1] + 16.0f < h - 0.5f) { skipped++; continue; }   // seluruh chunk di bawah air
    if (!cps || r.pid != curpid) {
      curpid = r.pid; cps = nullptr; cp = wprog_find(curpid);
      if (cp && cp->frame == g_frames && cp->usable) {
        cps = prog_stat(curpid);
        gx.UseProgram(curpid);
        gx.UniformMatrix4fv(cps->loc_wv, 1, 0, wv2);
        gx.UniformMatrix4fv(cps->loc_proj, 1, 0, pr2);
        if (cps->loc_vp >= 0 && cps->vp_state == 1) gx.Uniform3f(cps->loc_vp, cps->vp_orig[0], cps->vp_orig[1], cps->vp_orig[2]);
        for (int a = 0; a < 4; a++) if (cp->lay[a].ok) gx.EnableVertexAttribArray((unsigned)cp->lay[a].loc);
      }
    }
    if (!cps) { skipped++; continue; }
    gx.BindBuffer(0x8892, (unsigned)r.vbo);
    gx.BindBuffer(0x8893, (unsigned)r.ibo);
    int vs = 0, is = 0;
    gx.GetBufferParameteriv(0x8892, 0x8764, &vs);
    gx.GetBufferParameteriv(0x8893, 0x8764, &is);
    if (vs != r.vsz || is != r.isz || vs <= 0 || is <= 0) { skipped++; continue; }
    for (int a = 0; a < 4; a++) {
      const WLay& L = cp->lay[a];
      if (L.ok) gx.VertexAttribPointer((unsigned)L.loc, L.size, (unsigned)L.type, (unsigned char)L.norm, L.stride, (const void*)L.off);
    }
    gx.Uniform4f(cps->loc_origin, r.o[0], r.o[1], r.o[2], r.sc);
    gx.DrawElements(4, r.count, r.type, (const void*)r.idx);
    drawn++;
  }
  gx.FrontFace(0x0901);
  // pulihkan uniform program terrain
  for (int i = 0; i < g_nwp; i++) {
    WProg* p = &g_wp[i];
    if (p->frame != g_frames || !p->usable) continue;
    ProgStat* ps = prog_stat(p->id);
    gx.UseProgram(p->id);
    gx.UniformMatrix4fv(ps->loc_wv, 1, 0, p->wv);
    gx.UniformMatrix4fv(ps->loc_proj, 1, 0, p->pr);
    gx.Uniform4f(ps->loc_origin, p->org[0], p->org[1], p->org[2], p->org[3]);
    if (ps->loc_vp >= 0) gx.Uniform3f(ps->loc_vp, p->vp[0], p->vp[1], p->vp[2]);
  }
  gx.ClearColor(clr[0], clr[1], clr[2], clr[3]);
  restore_state(&st);
  unsigned err = gx.GetError();
  clear_gl_errors();
  g_w_passes++; g_w_draws += drawn; g_w_ms += (mono_sec() - t0) * 1000.0;
  if (g_w_logs < 3) {
    g_w_logs++;
    sh_log("air: pantulan frame %u h=%.3f camy=%.3f pemain_y=%.3f draw=%u lewati=%u err=0x%x\n", g_frames, h, g_w_camy, g_pl_y, drawn, skipped, err);
  } else if (g_frames % 900 == 0 && g_w_logs < 12) {
    g_w_logs++;
    sh_log("air: frame %u pass=%u draw/pass=%.0f ms/pass=%.2f h=%.3f camy=%.3f ready=%d\n", g_frames, g_w_passes, g_w_passes ? (double)g_w_draws / g_w_passes : 0.0,
           g_w_passes ? g_w_ms / g_w_passes : 0.0, h, g_w_camy, (int)g_w_ready);
    g_w_passes = 0; g_w_draws = 0; g_w_ms = 0;
  }
  return true;
}

// dipanggil sebelum draw blend (air, kaca) bawaan game; membuat pantulan sekali per frame
static void water_before_blend() {
  if (g_w_pass_frame == g_frames) return;
  g_w_pass_frame = g_frames;
  g_w_ready = false;
  if (g_w_stage < 1 || !g_gx_ok || !g_view_ok) return;
  float h = 0;
  if (!water_plane(&h)) return;
  g_w_h = h;
  int vp[4] = {0, 0, 0, 0};
  f_geti(0x0BA2, vp);
  if (vp[2] < 64 || vp[3] < 64) return;
  int w = (int)(vp[2] * g_w_scale), hh = (int)(vp[3] * g_w_scale);
  if (w < 64) w = 64;
  if (hh < 64) hh = 64;
  if (!g_w_armed && g_cfg_guard) { wguard_write(g_w_stage, 1); g_w_armed = true; }
  if (!water_resources(w, hh)) { g_w_stage = 0; sh_log("air: framebuffer pantulan gagal, dimatikan\n"); return; }
  if (!water_reflect_pass()) return;
  g_w_okframes++;
  if (g_w_armed && g_w_okframes >= 120) { wguard_write(g_w_stage, 0); g_w_armed = false; sh_log("air: stage %d lolos 120 frame pantulan tanpa crash\n", g_w_stage); }
  if (g_w_stage >= 2) g_w_ready = true;
}

// dipanggil sesudah draw blend bawaan: gambar ulang geometri yang sama dengan warna pantulan di ketinggian permukaan air
static void water_overlay(ProgStat* s, int count, unsigned type, const void* idx) {
  if (!g_w_ready || s->loc_origin < 0) return;
  float o[4] = {0, 0, 0, 1};
  f_unifv(s->id, s->loc_origin, o);
  if (o[1] > g_w_h + 0.3f || o[1] + 16.3f < g_w_h) return;   // hanya chunk yang dilewati bidang air
  int posloc = f_getaloc(s->id, "POSITION");
  if (posloc < 0) return;
  if (!g_w_prog || g_w_prog_pos != posloc) {
    if (!water_make_prog(posloc)) { g_w_ready = false; g_w_stage = 1; return; }
    g_w_uni_frame = 0xFFFFFFFFu;
  }
  int act = 0x84C0, prev3 = 0, bsr = 0x302, bdr = 0x303, bsa = 1, bda = 0x303, dfunc = 0x0201;
  unsigned char dmask = 1; float pf = 0, pu = 0; unsigned char poff = 0;
  f_geti(0x84E0, &act);
  f_geti(0x80C9, &bsr); f_geti(0x80C8, &bdr); f_geti(0x80CB, &bsa); f_geti(0x80CA, &bda);
  f_geti(0x0B74, &dfunc); f_getb(0x0B72, &dmask); poff = f_isen(0x8037);
  gx.GetFloatv(0x8038, &pf); gx.GetFloatv(0x2A00, &pu);
  gx.UseProgram(g_w_prog);
  if (g_w_uni_frame != g_frames) {
    g_w_uni_frame = g_frames;
    int vp[4] = {0, 0, 1, 1};
    f_geti(0x0BA2, vp);
    gx.UniformMatrix4fv(g_wu_wv, 1, 0, g_wv); gx.UniformMatrix4fv(g_wu_proj, 1, 0, g_proj);
    gx.Uniform1i(g_wu_refl, 3);
    gx.Uniform4f(g_wu_vp, (float)vp[0], (float)vp[1], (float)(vp[2] > 0 ? vp[2] : 1), (float)(vp[3] > 0 ? vp[3] : 1));
    gx.Uniform1f(g_wu_plane, g_w_h);
    gx.Uniform1f(g_wu_time, (float)fmod(mono_sec(), 6000.0));
    gx.Uniform2f(g_wu_camxz, (float)g_cam_est[0], (float)g_cam_est[2]);
    gx.Uniform3f(g_wu_skyh, g_w_fog[0], g_w_fog[1], g_w_fog[2]);
    float zr = g_w_fog[0] * 0.62f, zg = g_w_fog[1] * 0.80f, zb = g_w_fog[2] * 1.10f;
    gx.Uniform3f(g_wu_skyz, zr > 1 ? 1 : zr, zg > 1 ? 1 : zg, zb > 1 ? 1 : zb);
    gx.Uniform1f(g_wu_str, g_w_str); gx.Uniform1f(g_wu_tol, g_w_tol); gx.Uniform1f(g_wu_lift, g_w_lift);
  }
  gx.Uniform4f(g_wu_org, o[0], o[1], o[2], o[3]);
  gx.ActiveTexture(0x84C3); f_geti(0x8069, &prev3); gx.BindTexture(0x0DE1, g_w_tex);
  gx.BlendFuncSeparate(0x0302, 0x0303, 0, 1);
  gx.DepthMask(0); gx.DepthFunc(0x0203);
  gx.Enable(0x8037); gx.PolygonOffset(-1.0f, -1.0f);
  gx.DrawElements(4, count, type, idx);
  gx.BindTexture(0x0DE1, (unsigned)prev3);
  gx.ActiveTexture((unsigned)act);
  gx.BlendFuncSeparate((unsigned)bsr, (unsigned)bdr, (unsigned)bsa, (unsigned)bda);
  gx.DepthMask(dmask); gx.DepthFunc((unsigned)dfunc);
  set_cap(0x8037, poff); gx.PolygonOffset(pf, pu);
  gx.UseProgram(s->id);
  g_w_draws++;
}
// =========================================================================================

static void spy_use_program(GLuint_t p) {
  g_cur_prog = p;
  if (p) {
    ProgStat* s = prog_stat(p);
    if (s->kind == 0) {   // program terrain = yang punya uniform CHUNK_ORIGIN_AND_SCALE
      cap_init();
      int loc = f_getuloc ? f_getuloc(p, "CHUNK_ORIGIN_AND_SCALE") : -1;
      s->kind = loc >= 0 ? 1 : 2;
      s->loc_origin = loc;
      s->loc_wv = f_getuloc ? f_getuloc(p, "WORLDVIEW") : -1;
      s->loc_proj = f_getuloc ? f_getuloc(p, "PROJ") : -1;
      s->loc_wvp = f_getuloc ? f_getuloc(p, "WORLDVIEWPROJ") : -1;
      s->loc_cc = f_getuloc ? f_getuloc(p, "CURRENT_COLOR") : -1;
      s->loc_vp = f_getuloc ? f_getuloc(p, "VIEW_DIRECTION") : -1;   // uniform bawaan engine yang dipinjam untuk arah matahari
      s->loc_t2 = f_getuloc ? f_getuloc(p, "TEXTURE_2") : -1;
      s->vp_state = 0;
      s->loc_world = f_getuloc ? f_getuloc(p, "WORLD") : -1;
      if (s->kind == 2 && s->loc_world >= 0 && s->loc_wvp >= 0) s->kind = 3;   // program entity (mob, item, tangan): punya WORLD dan WORLDVIEWPROJ
    }
  }
  o_useprog(p);
}
static void spy_draw_elements(unsigned mode, int count, unsigned type, const void* idx) {
  ProgStat* s = prog_stat(g_cur_prog);
  s->draws++; s->verts += (unsigned)count; s->mode = mode; s->type = type;
  if (g_capturing) {
    capture_other(s, mode, count, s->kind != 1);
    if (s->kind == 1) capture_draw(s, mode, count, type, idx);
  }
  bool u2 = false;
  if (s->kind == 1) u2 = terrain_draw(s, count, type, idx);
  else if (s->kind == 2 && count == 6 && g_level >= 1 && g_tframe == g_frames) sky_draw(s, mode, count);
  else if (s->kind == 3) entity_draw(s, mode, 0, count, type, idx, true);
  bool wblend = false;
  if (s->kind == 1 && g_w_stage >= 1 && g_cur_fbo == 0 && g_tframe == g_frames && f_isen && f_isen(0x0BE2)) {
    wblend = true;   // draw blend (air): buat pantulan sekali per frame sebelum draw bawaan
    water_before_blend();
  }
  o_drawel(mode, count, type, idx);
  if (u2) terrain_draw_post();
  if (wblend && g_w_ready) water_overlay(s, count, type, idx);
}
static void spy_draw_arrays(unsigned mode, int first, int count) {
  ProgStat* s = prog_stat(g_cur_prog);
  s->draws++; s->verts += (unsigned)count; s->mode = mode; s->type = 0;
  if (g_capturing) capture_other(s, mode, count, s->kind != 1);
  if (s->kind == 3) entity_draw(s, mode, first, count, 0, nullptr, false);
  o_drawar(mode, first, count);
}
static void spy_bind_fb(unsigned target, GLuint_t fb) { g_cur_fbo = fb; g_fbo_binds++; o_bindfb(target, fb); }
static void spy_viewport(int x, int y, int w, int h) {
  g_vp_calls++;
  int i = 0;
  for (; i < g_nvp; i++) if (g_vp_w[i] == w && g_vp_h[i] == h) break;
  if (i == g_nvp && g_nvp < 8) { g_vp_w[i] = w; g_vp_h[i] = h; g_vp_n[i] = 0; g_nvp++; }
  if (i < g_nvp) g_vp_n[i]++;
  o_viewport(x, y, w, h);
}

typedef const unsigned char* (*gl_getstring_t)(unsigned);
typedef void (*gl_getprogramiv_t)(unsigned, unsigned, int*);
typedef void (*gl_getactive_t)(unsigned, unsigned, int, int*, int*, unsigned*, char*);
typedef void (*gl_getintegerv_t)(unsigned, int*);

static void describe_program(FILE* f, void* lib, unsigned id) {
  gl_getprogramiv_t giv = (gl_getprogramiv_t)dlsym(lib, "glGetProgramiv");
  gl_getactive_t gau = (gl_getactive_t)dlsym(lib, "glGetActiveUniform");
  gl_getactive_t gaa = (gl_getactive_t)dlsym(lib, "glGetActiveAttrib");
  if (!giv || !gau || !gaa) { fprintf(f, "  (fungsi query GL tidak ada)\n"); return; }
  int nu = 0, na = 0;
  giv(id, 0x8B86, &nu);   // GL_ACTIVE_UNIFORMS
  giv(id, 0x8B89, &na);   // GL_ACTIVE_ATTRIBUTES
  fprintf(f, "  attrib:");
  for (int i = 0; i < na && i < 16; i++) {
    char name[64] = {0}; int len = 0, size = 0; unsigned type = 0;
    gaa(id, i, 63, &len, &size, &type, name);
    fprintf(f, " %s(0x%x)", name, type);
  }
  fprintf(f, "\n  uniform:");
  for (int i = 0; i < nu && i < 40; i++) {
    char name[64] = {0}; int len = 0, size = 0; unsigned type = 0;
    gau(id, i, 63, &len, &size, &type, name);
    fprintf(f, " %s(0x%x)", name, type);
  }
  fprintf(f, "\n");
}

static unsigned spy_swap(void* dpy, void* surf) {
  g_frames++;
  if (g_capturing) {
    g_capturing = false;
    if (g_cap_f) { fprintf(g_cap_f, "T2 selesai frame=%u, total draw terrain=%u\n", g_frames, g_cap_draws); fclose(g_cap_f); g_cap_f = nullptr; }
  }
  {
    double tnow = mono_sec();
    if (g_next_cap == 0) g_next_cap = tnow + 60.0;
    if (!g_cfg_loaded) cfg_load();
    if (g_cfg_capture && tnow >= g_next_cap && g_cap_count < 30) {
      g_cap_count++;
      g_next_cap = tnow + 45.0;
      g_cap_f = fopen("/sdcard/games/com.mojang/pipe3d_log.txt", "a");
      if (g_cap_f) {
        g_capturing = true; g_cap_draws = 0; g_cap_mat = false; g_seq = 0; g_cap_other = 0;
        g_cap_full = (g_cap_count == 1);
        fprintf(g_cap_f, "T2 mulai frame=%u capture=%u\n", g_frames, g_cap_count);
      }
    }
  }
  void* lib = dlopen("libGLESv2.so", RTLD_NOW);
  if (!g_gl_info_done && g_frames >= 60 && lib) {
    g_gl_info_done = true;
    gl_getstring_t gs = (gl_getstring_t)dlsym(lib, "glGetString");
    gl_getintegerv_t gi = (gl_getintegerv_t)dlsym(lib, "glGetIntegerv");
    FILE* f = fopen("/sdcard/games/com.mojang/pipe3d_log.txt", "a");
    if (f && gs) {
      const char* ver = (const char*)gs(0x1F02);
      const char* ren = (const char*)gs(0x1F01);
      const char* ext = (const char*)gs(0x1F03);
      fprintf(f, "GL version=%s renderer=%s\n", ver ? ver : "?", ren ? ren : "?");
      if (ext) {
        fprintf(f, "GL ext: depth_texture=%d depth24=%d shadow_samplers=%d standard_derivatives=%d texture_half_float=%d\n",
                strstr(ext, "OES_depth_texture") != nullptr, strstr(ext, "OES_depth24") != nullptr,
                strstr(ext, "EXT_shadow_samplers") != nullptr, strstr(ext, "OES_standard_derivatives") != nullptr,
                strstr(ext, "OES_texture_half_float") != nullptr);
      }
      int units = 0; if (gi) gi(0x8872, &units);   // GL_MAX_TEXTURE_IMAGE_UNITS
      fprintf(f, "GL max texture units=%d\n", units);
    }
    if (f) fclose(f);
  }
  if (g_frames % 300 == 0 && g_summaries < 1) {
    g_summaries++;
    FILE* f = fopen("/sdcard/games/com.mojang/pipe3d_log.txt", "a");
    if (f) {
      fprintf(f, "== GL ringkasan %u (frame %u): bindFramebuffer=%u, viewport=%u, fbo terakhir=%u\n",
              g_summaries, g_frames, g_fbo_binds, g_vp_calls, g_cur_fbo);
      for (int i = 0; i < g_nvp; i++) fprintf(f, " viewport %dx%d: %u kali\n", g_vp_w[i], g_vp_h[i], g_vp_n[i]);
      for (int i = 0; i < g_np; i++) {
        ProgStat* s = &g_prog[i];
        if (!s->draws) continue;
        fprintf(f, " program %u: draw=%u verts=%u mode=0x%x type=0x%x\n", s->id, s->draws, s->verts, s->mode, s->type);
        if (!s->described && lib) { s->described = true; describe_program(f, lib, s->id); }
        s->draws = 0; s->verts = 0;
      }
      fclose(f);
    }
    g_fbo_binds = 0; g_vp_calls = 0;
    for (int i = 0; i < g_nvp; i++) g_vp_n[i] = 0;
  }
  shadow_end_frame();
  return o_swap(dpy, surf);
}

struct GotHook { const char* name; void* hook; void** orig; bool found; };
static GotHook g_got_hooks[] = {
  {"glUseProgram", (void*)spy_use_program, (void**)&o_useprog, false},
  {"glDrawElements", (void*)spy_draw_elements, (void**)&o_drawel, false},
  {"glDrawArrays", (void*)spy_draw_arrays, (void**)&o_drawar, false},
  {"glBindFramebuffer", (void*)spy_bind_fb, (void**)&o_bindfb, false},
  {"glViewport", (void*)spy_viewport, (void**)&o_viewport, false},
  {"eglSwapBuffers", (void*)spy_swap, (void**)&o_swap, false},
};

struct LibInfo { uintptr_t base; const Elf32_Phdr* ph; int n; };
static int find_lib_cb(struct dl_phdr_info* info, size_t, void* data) {
  if (info->dlpi_name && strstr(info->dlpi_name, "libminecraftpe.so")) {
    LibInfo* li = (LibInfo*)data;
    li->base = (uintptr_t)info->dlpi_addr;
    li->ph = (const Elf32_Phdr*)info->dlpi_phdr;
    li->n = info->dlpi_phnum;
    return 1;
  }
  return 0;
}

static void patch_got_entry(uint32_t* got, void* hook, void** orig) {
  if (*orig == nullptr) *orig = (void*)(uintptr_t)(*got);
  uintptr_t page = (uintptr_t)got & ~(uintptr_t)4095;
  if (mprotect((void*)page, 4096, PROT_READ | PROT_WRITE) != 0) return;
  *got = (uint32_t)(uintptr_t)hook;
  mprotect((void*)page, 4096, PROT_READ);
}

static void install_gl_spy() {
  LibInfo li = {0, nullptr, 0};
  dl_iterate_phdr(find_lib_cb, &li);
  if (!li.base || !li.ph) { LOG("spy: lib tidak ditemukan"); return; }
  const Elf32_Dyn* dyn = nullptr;
  for (int i = 0; i < li.n; i++) if (li.ph[i].p_type == PT_DYNAMIC) { dyn = (const Elf32_Dyn*)(li.base + li.ph[i].p_vaddr); break; }
  if (!dyn) { LOG("spy: PT_DYNAMIC tidak ada"); return; }
  uintptr_t symtab = 0, strtab = 0, jmprel = 0, rel = 0; uint32_t pltrelsz = 0, relsz = 0;
  for (const Elf32_Dyn* d = dyn; d->d_tag != DT_NULL; d++) {
    uintptr_t v = d->d_un.d_ptr;
    if (d->d_tag == DT_SYMTAB) symtab = v;
    else if (d->d_tag == DT_STRTAB) strtab = v;
    else if (d->d_tag == DT_JMPREL) jmprel = v;
    else if (d->d_tag == DT_PLTRELSZ) pltrelsz = d->d_un.d_val;
    else if (d->d_tag == DT_REL) rel = v;
    else if (d->d_tag == DT_RELSZ) relsz = d->d_un.d_val;
  }
  // Pada beberapa versi Android, alamat di tabel dinamis sudah mutlak; pada yang lain masih relatif
  if (symtab && symtab < li.base) symtab += li.base;
  if (strtab && strtab < li.base) strtab += li.base;
  if (jmprel && jmprel < li.base) jmprel += li.base;
  if (rel && rel < li.base) rel += li.base;
  if (!symtab || !strtab) { LOG("spy: symtab/strtab kosong"); return; }
  struct Sect { uintptr_t a; uint32_t sz; } sects[2] = {{jmprel, pltrelsz}, {rel, relsz}};
  int patched = 0;
  for (int s = 0; s < 2; s++) {
    if (!sects[s].a) continue;
    const Elf32_Rel* r = (const Elf32_Rel*)sects[s].a;
    int n = sects[s].sz / sizeof(Elf32_Rel);
    for (int i = 0; i < n; i++) {
      unsigned type = ELF32_R_TYPE(r[i].r_info), sym = ELF32_R_SYM(r[i].r_info);
      if (type != 21 && type != 22) continue;   // R_ARM_GLOB_DAT, R_ARM_JUMP_SLOT
      if (!sym) continue;
      const Elf32_Sym* sy = (const Elf32_Sym*)symtab + sym;
      const char* name = (const char*)strtab + sy->st_name;
      for (unsigned k = 0; k < sizeof(g_got_hooks) / sizeof(g_got_hooks[0]); k++) {
        if (strcmp(name, g_got_hooks[k].name) == 0) {
          patch_got_entry((uint32_t*)(li.base + r[i].r_offset), g_got_hooks[k].hook, g_got_hooks[k].orig);
          g_got_hooks[k].found = true; patched++;
        }
      }
    }
  }
  for (unsigned k = 0; k < sizeof(g_got_hooks) / sizeof(g_got_hooks[0]); k++)
    LOG("spy: %s %s", g_got_hooks[k].name, g_got_hooks[k].found ? "dipasang" : "TIDAK ditemukan");
  FILE* f = fopen("/sdcard/games/com.mojang/pipe3d_log.txt", "a");
  if (f) {
    fprintf(f, "spy GL: dipasang %d entri GOT;", patched);
    for (unsigned k = 0; k < sizeof(g_got_hooks) / sizeof(g_got_hooks[0]); k++)
      fprintf(f, " %s=%d", g_got_hooks[k].name, (int)g_got_hooks[k].found);
    fprintf(f, "\n");
    fclose(f);
  }
}

// ===================== GUNUNG TINGGI (patch biome) =====================
// Biome::setDepthAndScale(BiomeHeight const&) hanya 5 instruksi Thumb:
//   ldr r2,[r1]; str r2,[r0,#0x64]; ldr r1,[r1,#4]; str r1,[r0,#0x68]; bx lr
// 8 byte pertama (4 instruksi) tidak bergantung posisi, jadi aman dipindah ke trampolin install_hook.
// depth di Biome+0x64, scale di Biome+0x68. Tinggi dunia tetap maksimal 127, jadi puncak dibatasi.
static const uintptr_t OFF_SET_DEPTH_SCALE = 0xCA5FB5;   // Biome::setDepthAndScale
static const uint8_t SIG_SET_DEPTH_SCALE[8] = {0x0a, 0x68, 0x42, 0x66, 0x49, 0x68, 0x81, 0x66};
static const uintptr_t OFF_HEIGHTS_MOUNTAINS = 0x1704C08; // Biome::HEIGHTS_MOUNTAINS {depth, scale}
static const uintptr_t OFF_HEIGHTS_HIGHLANDS = 0x1704C10; // Biome::HEIGHTS_HIGHLANDS
static const uintptr_t OFF_HEIGHTS_EXTREME   = 0x1704C18; // Biome::HEIGHTS_EXTREME
static const uintptr_t OFF_BIOMES = 0x1704D58;            // Biome::mBiomes[256] (penunjuk Biome*)
static const int BIOME_DEPTH_OFF = 0x64;
static const int BIOME_SCALE_OFF = 0x68;

typedef void (*SetDepthScaleFn)(void* self, const float* h);
static SetDepthScaleFn orig_set_depth_scale = nullptr;
static uintptr_t g_base = 0;
static float g_depth_mul = 1.0f, g_depth_add = 0.4f, g_scale_mul = 1.6f;
static float g_depth_max = 2.5f, g_scale_max = 2.0f;
static int g_biome_enabled = 1;

static void load_biome_config() {
  FILE* f = fopen("/sdcard/games/com.mojang/pipe3d_biome.txt", "r");
  if (!f) return;
  char line[128];
  while (fgets(line, sizeof(line), f)) {
    char key[40]; float v;
    if (sscanf(line, " %39[^=]=%f", key, &v) != 2) continue;
    if (!strcmp(key, "enabled")) g_biome_enabled = (int)v;
    else if (!strcmp(key, "depth_mul")) g_depth_mul = v;
    else if (!strcmp(key, "depth_add")) g_depth_add = v;
    else if (!strcmp(key, "scale_mul")) g_scale_mul = v;
    else if (!strcmp(key, "depth_max")) g_depth_max = v;
    else if (!strcmp(key, "scale_max")) g_scale_max = v;
  }
  fclose(f);
}

// Apakah (d,s) sama dengan salah satu tinggi gunung bawaan?
static bool is_mountain_height(float d, float s) {
  const uintptr_t offs[3] = {OFF_HEIGHTS_MOUNTAINS, OFF_HEIGHTS_HIGHLANDS, OFF_HEIGHTS_EXTREME};
  for (int i = 0; i < 3; i++) {
    const float* h = (const float*)(g_base + offs[i]);
    if (h[0] == d && h[1] == s) return true;
  }
  return false;
}

static void boost_biome(uint8_t* biome) {
  float* pd = (float*)(biome + BIOME_DEPTH_OFF);
  float* ps = (float*)(biome + BIOME_SCALE_OFF);
  float d = *pd * g_depth_mul + g_depth_add;
  float s = *ps * g_scale_mul;
  if (d > g_depth_max) d = g_depth_max;
  if (s > g_scale_max) s = g_scale_max;
  *pd = d; *ps = s;
}

static void hook_set_depth_scale(void* self, const float* h) {
  orig_set_depth_scale(self, h);
  if (g_biome_enabled && is_mountain_height(h[0], h[1])) boost_biome((uint8_t*)self);
}

// Cadangan kalau initBiomes sudah jalan sebelum hook terpasang: periksa biome yang sudah ada.
// Aman dijalankan berulang: biome yang sudah dinaikkan nilainya tidak lagi sama dengan nilai bawaan.
static void* biome_sweep(void*) {
  bool logged = false;
  for (int round = 0; round < 40; round++) {
    sleep(3);
    for (int i = 0; i < 256; i++) {
      uint8_t* b = *(uint8_t**)(g_base + OFF_BIOMES + i * 4);
      if (!b) continue;
      float d = *(float*)(b + BIOME_DEPTH_OFF), s = *(float*)(b + BIOME_SCALE_OFF);
      if (!logged) {
        FILE* f = fopen("/sdcard/games/com.mojang/pipe3d_biome_log.txt", "a");
        if (f) { fprintf(f, "biome id=%d depth=%f scale=%f gunung=%d\n", i, d, s, (int)is_mountain_height(d, s)); fclose(f); }
      }
      if (g_biome_enabled && is_mountain_height(d, s)) {
        boost_biome(b);
        FILE* f = fopen("/sdcard/games/com.mojang/pipe3d_biome_log.txt", "a");
        if (f) { fprintf(f, "sweep: biome id=%d dinaikkan\n", i); fclose(f); }
      }
    }
    logged = true;
  }
  return nullptr;
}

static void install_biome_patch(uintptr_t base) {
  g_base = base;
  load_biome_config();
  uintptr_t target = base + OFF_SET_DEPTH_SCALE;
  if (memcmp((void*)(target & ~(uintptr_t)1), SIG_SET_DEPTH_SCALE, 8) != 0) {
    LOG("biome: tanda fungsi tidak cocok, patch gunung dibatalkan");
    return;
  }
  if (install_hook(target & ~(uintptr_t)1, (void*)hook_set_depth_scale, (void**)&orig_set_depth_scale))
    LOG("biome: hook setDepthAndScale terpasang (enabled=%d depth_add=%.2f scale_mul=%.2f)", g_biome_enabled, g_depth_add, g_scale_mul);
  else
    LOG("biome: hook gagal dipasang");
  pthread_t th;
  pthread_create(&th, nullptr, biome_sweep, nullptr);
  pthread_detach(th);
}
// =======================================================================

// ===================== LENTERA (blok baru) =====================
// Tiga blok BARU di ID kosong (bukan mengganti blok lain): lantern (berdiri), chain (rantai), hanging_lantern (gantung).
// Urutan kerja (semua lewat hook di awal-akhir fungsi game):
//  1. Setelah Block::initBlocks(): daftarkan blok lewat registerBlock<Block,...>, atur cahaya, kategori, bentuk.
//     Item otomatis dibuat game di Item::addBlockItems (ia memeriksa semua blok yang terdaftar).
//  2. Setelah BlockGraphics::initBlocks(): daftarkan gambar blok dari JSON kecil di dalam kode (tanpa edit APK).
//  3. Setelah Item::initCreativeItems(): tambahkan lentera ke inventory kreatif.
//  4. Di hook_tess_block (atas): lentera digambar dari 3 kotak (badan, tutup, cincin), cara yang sama seperti meja kerja.
// Pengaman: baru jalan kalau /sdcard/games/com.mojang/pipe3d_lantern.txt ada dan berisi enabled=1.
// Tiap bagian bisa dimatikan sendiri di file itu: item=0 graphics=0 creative=0 shape=0 (untuk mencari penyebab crash).
static const uintptr_t OFF_INIT_BLOCKS = 0xCB2CD9;          // Block::initBlocks()
static const uint8_t SIG_INIT_BLOCKS[10] = {0x2d,0xe9,0xf0,0x4f,0x81,0xb0,0x2d,0xed,0x02,0x8b}; // push.w, sub sp, vpush (10 byte utuh)
static const uintptr_t OFF_BG_INIT_BLOCKS = 0x98A355;       // BlockGraphics::initBlocks()
static const uint8_t SIG_BG_INIT_BLOCKS[8] = {0x2d,0xe9,0xf0,0x4f,0xad,0xf5,0x19,0x7d};
static const uintptr_t OFF_INIT_CREATIVE = 0xC79A31;        // Item::initCreativeItems()
static const uint8_t SIG_INIT_CREATIVE[8] = {0x2d,0xe9,0xf0,0x4f,0xad,0xf5,0xef,0x5d};
static const uintptr_t OFF_REGISTER_BLOCK_8 = 0xCB71B1;     // registerBlock<Block, char const(&)[8], int, Material const&>
static const uintptr_t OFF_REGISTER_GFX_8 = 0x98E0FD;       // registerBlock<BlockGraphics, char const(&)[8]>(Json::Value const&, ...)
static const uintptr_t OFF_GET_MATERIAL = 0xD7A999;         // Material::getMaterial(MaterialType)
static const uintptr_t OFF_SET_LIGHT_EMISSION = 0xCC3F25;   // Block::setLightEmission(float)
static const uintptr_t OFF_SET_DESTROY_TIME = 0xCC51A9;     // Block::setDestroyTime(float)
static const uintptr_t OFF_SET_VISUAL_SHAPE = 0xCC5211;     // Block::setVisualShape(Vec3 const&, Vec3 const&)
static const uintptr_t OFF_SET_CATEGORY = 0xCB6BA5;         // Block::setCategory(CreativeItemCategory)
static const uintptr_t OFF_GET_CATEGORY = 0xCC3EE5;         // Block::getCreativeCategory()
static const uintptr_t OFF_ADD_CREATIVE = 0xC87715;         // Item::addCreativeItem(ItemInstance const&)
static const uintptr_t OFF_ITEMINSTANCE_IIII = 0xC93E91;    // ItemInstance::ItemInstance(int id, int count, int aux)
static const uintptr_t OFF_JSON_VALUE_CTOR = 0x119BC35;     // Json::Value::Value(ValueType)
static const uintptr_t OFF_JSON_VALUE_DTOR = 0x119BF6D;     // Json::Value::~Value()
static const uintptr_t OFF_JSON_READER_CTOR = 0x119893D;    // Json::Reader::Reader()
static const uintptr_t OFF_JSON_READER_PARSE = 0x1198BD9;   // Json::Reader::parse(char const*, char const*, Value&, bool)
static const uintptr_t OFF_MBLOCKS = 0x1705814;             // Block::mBlocks[256]
static const uintptr_t OFF_BG_MBLOCKS = 0x16B350C;          // BlockGraphics::mBlocks[256]
static const uintptr_t OFF_ITEM_MITEMS = 0x1703A00;         // Item::mItems[512]
static const int ID_GLOWSTONE = 89;                         // gambar cadangan kalau pendaftaran gambar gagal
static const uintptr_t OFF_MSOLID = 0x1705E3C;              // Block::mSolid[256]
static const uintptr_t OFF_MLIGHTBLOCK = 0x1705C3C;         // Block::mLightBlock[256]
static const int MATERIAL_TYPE_METAL = 4;                   // urutan: air0 dirt1 wood2 stone3 metal4 (cocok dengan blok yang sudah ada)
static const int CATEGORY_DECORATIONS = 2;                  // CreativeItemCategory: 1 blok, 2 dekorasi, 3 alat, 4 item

// Gambar blok (format sama dengan blocks.json). registerBlock<BlockGraphics>(root, nama) mengambil root[nama] sendiri,
// jadi JSON dibungkus dengan nama tiap blok di tingkat paling luar.
// Versi bawaan (texture=0): memakai tekstur yang sudah ada di game.
static const char LT_GFX_JSON_BASIC[] =
  "{\"lantern\":{\"textures\":{\"up\":\"iron_block\",\"down\":\"iron_block\",\"side\":\"glowstone\"}},"
  "\"chain\":{\"textures\":\"iron_block\"},"
  "\"hanging_lantern\":{\"textures\":{\"up\":\"iron_block\",\"down\":\"iron_block\",\"side\":\"glowstone\"}},"
  "\"campfire\":{\"textures\":\"glowstone\"}}";
// Versi tekstur sendiri (texture=1): gambar harus sudah ada di APK (lihat lantern_texture_pack.zip).
static const char LT_GFX_JSON_CUSTOM[] =
  "{\"lantern\":{\"textures\":{\"up\":\"lantern_top\",\"down\":\"lantern_top\",\"side\":\"lantern_side\"},"
  "\"carried_textures\":{\"up\":\"lantern_top\",\"down\":\"lantern_top\",\"side\":\"lantern_icon\"}},"
  "\"chain\":{\"textures\":{\"up\":\"chain_top\",\"down\":\"chain_top\",\"side\":\"chain_side\"},"
  "\"carried_textures\":{\"up\":\"chain_top\",\"down\":\"chain_top\",\"side\":\"chain_icon\"}},"
  "\"hanging_lantern\":{\"textures\":{\"up\":\"lantern_top\",\"down\":\"lantern_top\",\"side\":\"lantern_hang_side\"},"
  "\"carried_textures\":{\"up\":\"lantern_top\",\"down\":\"lantern_top\",\"side\":\"lantern_icon\"}},"
  "\"campfire\":{\"textures\":{\"up\":\"campfire_log_top\",\"down\":\"campfire_log_top\",\"side\":\"campfire_log_side\"},"
  "\"carried_textures\":{\"up\":\"campfire_log_top\",\"down\":\"campfire_log_top\",\"side\":\"campfire_icon\"}}}";
static int g_lt_texture = 0;
static const char* LT_NAMES[LT_COUNT] = {"lantern", "chain", "hanging_lantern", "campfire"};
static const float LT_LIGHT[LT_COUNT] = {1.0f, 0.0f, 1.0f, 1.0f};
static const int LT_MATERIAL[LT_COUNT] = {4, 4, 4, 2};            // 4 = metal, 2 = kayu (api unggun)
static const float LT_HARDNESS[LT_COUNT] = {1.0f, 1.0f, 1.0f, 2.0f};
// Kotak seleksi (outline) tiap blok, satuan 1/16: xmin,ymin,zmin,xmax,ymax,zmax
static const float LT_OUTLINE[LT_COUNT][6] = {
  // unggun: ymin=1 (bukan 0) sengaja. Game menyembunyikan sisi atas blok di bawah unggun kalau kotak bentuk unggun menempel dan menutupi
  // seluruh sisi itu (cek ymin<=0 dan x/z 0..1 di BlockOccluder::_shouldRenderFace), padahal model kayunya berlubang, jadi blok bawah tembus pandang.
  {5, 0, 5, 11, 10, 11}, {6, 0, 6, 10, 16, 10}, {5, 6, 5, 11, 16, 11}, {0, 1, 0, 16, 7, 16},
};

typedef void (*InitVoidFn)();
typedef void* (*RegBlock8Fn)(const char (*name)[8], int* id, const void* material);
typedef const void* (*GetMaterialFn)(int type);
typedef void* (*SetFloatFn)(void* block, float v);
typedef void* (*SetIntFn)(void* block, int v);
typedef int (*GetIntFn)(void* block);
typedef void* (*SetShapeFn)(void* block, const float* vmin, const float* vmax);
typedef void (*JsonValueCtorFn)(void* self, int type);
typedef void (*JsonValueDtorFn)(void* self);
typedef void (*JsonReaderCtorFn)(void* self);
typedef bool (*JsonParseFn)(void* reader, const char* b, const char* e, void* root, bool comments);
typedef void* (*RegGfx8Fn)(const void* json, const char (*name)[8]);
typedef void (*ItemInstanceCtorFn)(void* self, int id, int count, int aux);
typedef void (*AddCreativeFn)(const void* inst);
static InitVoidFn orig_init_blocks = nullptr;
static InitVoidFn orig_bg_init_blocks = nullptr;
static InitVoidFn orig_init_creative = nullptr;
static int g_lt_item = 1, g_lt_graphics = 1, g_lt_creative = 1;
static int g_fire_smoke = 1, g_fire_flame = 1;   // efek api unggun (smoke=0 mematikan semua, flame=0 hanya percikan api)
static int g_lt_cfg_id[LT_COUNT] = {210, 211, 212, 213};   // slot ID (200..242 tidak dipakai blok bawaan 0.15.10)

static void lantern_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void lantern_log(const char* fmt, ...) {
  FILE* f = fopen("/sdcard/games/com.mojang/pipe3d_lantern_log.txt", "a");
  if (!f) return;
  va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
  fputc('\n', f); fclose(f);
}

static bool lantern_enabled() {
  FILE* f = fopen("/sdcard/games/com.mojang/pipe3d_lantern.txt", "r");
  if (!f) return false;
  char line[64]; bool on = false;
  while (fgets(line, sizeof(line), f)) {
    int v;
    if (sscanf(line, " enabled=%d", &v) == 1) on = (v != 0);
    else if (sscanf(line, " item=%d", &v) == 1) g_lt_item = v;
    else if (sscanf(line, " graphics=%d", &v) == 1) g_lt_graphics = v;
    else if (sscanf(line, " creative=%d", &v) == 1) g_lt_creative = v;
    else if (sscanf(line, " shape=%d", &v) == 1) g_lantern_shape = v;
    else if (sscanf(line, " id=%d", &v) == 1) g_lt_cfg_id[0] = v;
    else if (sscanf(line, " chain_id=%d", &v) == 1) g_lt_cfg_id[1] = v;
    else if (sscanf(line, " hang_id=%d", &v) == 1) g_lt_cfg_id[2] = v;
    else if (sscanf(line, " fire_id=%d", &v) == 1) g_lt_cfg_id[3] = v;
    else if (sscanf(line, " smoke=%d", &v) == 1) g_fire_smoke = v;
    else if (sscanf(line, " flame=%d", &v) == 1) g_fire_flame = v;
    else if (sscanf(line, " texture=%d", &v) == 1) g_lt_texture = v;
  }
  fclose(f);
  return on;
}

static void register_one(int k) {
  void** blocks = (void**)(g_base + OFF_MBLOCKS);
  // Di akhir initBlocks game mengisi SEMUA ID yang kosong dengan blok pengganti bernama "" (itu sebabnya
  // tidak ada ID null). Jadi kita ambil satu slot pengganti, kosongkan penunjuknya, lalu daftarkan blok di situ.
  int id = g_lt_cfg_id[k];
  if (id < 1 || id > 255) id = 210 + k;
  void* old_block = blocks[id];
  if (old_block && blocks[255] && *(void**)old_block != *(void**)blocks[255]) {
    lantern_log("[%s] slot id=%d bukan blok pengganti (jenis berbeda), dibatalkan; ubah id di pipe3d_lantern.txt", LT_NAMES[k], id);
    return;
  }
  blocks[id] = nullptr;   // konstruktor Block mengisi slot ini lagi dengan blok baru
  GetMaterialFn get_material = (GetMaterialFn)(g_base + OFF_GET_MATERIAL);
  RegBlock8Fn reg = (RegBlock8Fn)(g_base + OFF_REGISTER_BLOCK_8);
  const void* mat = get_material(LT_MATERIAL[k]);
  if (!mat) { blocks[id] = old_block; lantern_log("[%s] material tidak ditemukan, dibatalkan", LT_NAMES[k]); return; }
  char name[32];
  memset(name, 0, sizeof(name));
  strncpy(name, LT_NAMES[k], sizeof(name) - 1);   // fungsi game membaca nama sampai karakter NUL, jadi panjang larik tidak masalah
  int idv = id;
  void* blk = reg((const char(*)[8])name, &idv, mat);
  if (!blk) { blocks[id] = old_block; lantern_log("[%s] registerBlock mengembalikan null", LT_NAMES[k]); return; }
  if (LT_LIGHT[k] > 0.0f) ((SetFloatFn)(g_base + OFF_SET_LIGHT_EMISSION))(blk, LT_LIGHT[k]);   // cahaya
  ((SetFloatFn)(g_base + OFF_SET_DESTROY_TIME))(blk, LT_HARDNESS[k]);   // kekerasan
  int cat = CATEGORY_DECORATIONS;
  if (blocks[50]) cat = ((GetIntFn)(g_base + OFF_GET_CATEGORY))(blocks[50]);   // samakan dengan obor: pasti tab yang ada
  if (g_lt_item) ((SetIntFn)(g_base + OFF_SET_CATEGORY))(blk, cat);            // dibaca Item::addBlockItems
  if (g_lantern_shape) {
    ((uint8_t*)(g_base + OFF_MSOLID))[id] = 0;        // bukan kubus penuh: sisi blok tetangga tetap digambar
    ((uint8_t*)(g_base + OFF_MLIGHTBLOCK))[id] = 0;   // tidak menghalangi cahaya
    const float vmin[3] = {LT_OUTLINE[k][0] / 16, LT_OUTLINE[k][1] / 16, LT_OUTLINE[k][2] / 16};
    const float vmax[3] = {LT_OUTLINE[k][3] / 16, LT_OUTLINE[k][4] / 16, LT_OUTLINE[k][5] / 16};
    ((SetShapeFn)(g_base + OFF_SET_VISUAL_SHAPE))(blk, vmin, vmax);   // kotak seleksi
  }
  g_lt_ids[k] = id;
  lantern_log("[%s] terdaftar: id=%d blok=%p mBlocks[id]=%p (pengganti lama=%p) kategori=%d item=%d graphics=%d creative=%d shape=%d",
              LT_NAMES[k], id, blk, blocks[id], old_block, cat, g_lt_item, g_lt_graphics, g_lt_creative, g_lantern_shape);
}

static void register_lantern() {
  for (int k = 0; k < LT_COUNT; k++) register_one(k);
}

static void register_lantern_graphics() {
  alignas(8) uint8_t reader[512];
  alignas(8) uint8_t root[64];
  memset(reader, 0, sizeof(reader));
  memset(root, 0, sizeof(root));
  ((JsonReaderCtorFn)(g_base + OFF_JSON_READER_CTOR))(reader);
  ((JsonValueCtorFn)(g_base + OFF_JSON_VALUE_CTOR))(root, 0);
  const char* js = g_lt_texture ? LT_GFX_JSON_CUSTOM : LT_GFX_JSON_BASIC;
  size_t jl = g_lt_texture ? sizeof(LT_GFX_JSON_CUSTOM) - 1 : sizeof(LT_GFX_JSON_BASIC) - 1;
  bool ok = ((JsonParseFn)(g_base + OFF_JSON_READER_PARSE))(reader, js, js + jl, root, false);
  if (!ok) { lantern_log("gambar: JSON tidak terbaca"); return; }
  void** gt = (void**)(g_base + OFF_BG_MBLOCKS);
  for (int k = 0; k < LT_COUNT; k++) {
    if (g_lt_ids[k] < 0) continue;
    char name[32];
    memset(name, 0, sizeof(name));
    strncpy(name, LT_NAMES[k], sizeof(name) - 1);
    void* gfx = ((RegGfx8Fn)(g_base + OFF_REGISTER_GFX_8))(root, (const char(*)[8])name);
    lantern_log("gambar [%s] (texture=%d): registerBlock=%p slot=%p", LT_NAMES[k], g_lt_texture, gfx, gt[g_lt_ids[k]]);
    if (gfx && !gt[g_lt_ids[k]]) gt[g_lt_ids[k]] = gfx;   // jaga-jaga kalau slot belum terisi
  }
  ((JsonValueDtorFn)(g_base + OFF_JSON_VALUE_DTOR))(root);
}

// Pengaman: ikon di inventory membaca BlockGraphics::mBlocks[id]; kalau kosong, game crash saat ikon digambar.
static void ensure_lantern_graphics_slot() {
  void** gt = (void**)(g_base + OFF_BG_MBLOCKS);
  for (int k = 0; k < LT_COUNT; k++) {
    if (g_lt_ids[k] < 0) continue;
    if (!gt[g_lt_ids[k]]) {
      gt[g_lt_ids[k]] = gt[ID_GLOWSTONE];   // cadangan: pakai gambar glowstone
      lantern_log("gambar [%s] kosong; memakai gambar glowstone sebagai cadangan (%p)", LT_NAMES[k], gt[g_lt_ids[k]]);
    }
  }
}

static void add_lantern_creative() {
  void** items = (void**)(g_base + OFF_ITEM_MITEMS);
  void** gt = (void**)(g_base + OFF_BG_MBLOCKS);
  for (int k = 0; k < LT_COUNT; k++) {
    int id = g_lt_ids[k];
    if (id < 0) continue;
    lantern_log("[%s] cek sebelum masuk kreatif: item=%p gambar=%p", LT_NAMES[k], items[id], gt[id]);
    if (!items[id]) { lantern_log("[%s] item tidak dibuat game; tidak dimasukkan ke inventory (mencegah crash)", LT_NAMES[k]); continue; }
    if (!gt[id]) { lantern_log("[%s] gambar kosong; tidak dimasukkan ke inventory (mencegah crash)", LT_NAMES[k]); continue; }
    alignas(8) uint8_t inst[64];
    memset(inst, 0, sizeof(inst));
    ((ItemInstanceCtorFn)(g_base + OFF_ITEMINSTANCE_IIII))(inst, id, 1, 0);
    ((AddCreativeFn)(g_base + OFF_ADD_CREATIVE))(inst);
    lantern_log("[%s] masuk inventory kreatif (id=%d)", LT_NAMES[k], id);
  }
}

// ===================== API UNGGUN: asap dan percikan api =====================
// Blok api unggun adalah Block biasa (tidak punya animateTick sendiri), jadi efeknya dibuat dari luar:
//  - hook_tess_block mencatat posisi tiap api unggun yang digambar (daftar kecil, maksimal 64).
//  - Level::animateTick(Entity&) dipanggil game tiap tick untuk pemain; di akhirnya kita lewati daftar itu,
//    cek blok masih ada (BlockSource::getBlockID), lalu panggil Level::addParticle untuk asap dan api kecil.
// Tipe partikel dibaca dari obor dan tungku bawaan (animateTick keduanya memanggil addParticle dengan 3 lalu 6): 3 = asap, 6 = api.
// Dimatikan lewat pipe3d_lantern.txt: smoke=0 (semua efek), flame=0 (hanya percikan api).
static const uintptr_t OFF_LEVEL_ANIMATE_TICK = 0xD27A59;   // Level::animateTick(Entity&)
static const uint8_t SIG_LEVEL_ANIMATE_TICK[8] = {0x2d,0xe9,0xf0,0x4f,0x8b,0xb0,0x0d,0x46};  // push.w, sub sp, mov r5,r1 (8 byte utuh)
static const uintptr_t OFF_LEVEL_ADD_PARTICLE = 0xD26695;   // Level::addParticle(ParticleType, Vec3 const&, Vec3 const&, int)
static const uintptr_t OFF_BS_GET_BLOCK_ID_XYZ = 0xD102D5;  // BlockSource::getBlockID(int, int, int)
static const int ENTITY_POS_OFF = 8;        // Entity::getPos() hanya "adds r0,#8": Vec3 posisi ada di entity+8
static const int ENTITY_REGION_OFF = 0x194; // Entity::getRegion() hanya membaca entity+0x194 (BlockSource*)
static const int PARTICLE_SMOKE = 3;
static const int PARTICLE_FLAME = 6;
static const int FIRE_MAX = 64;
static const float FIRE_RANGE = 24.0f;      // efek hanya dibuat kalau pemain dalam jarak ini
static int g_fire_pos[FIRE_MAX][3];
static int g_fire_cnt = 0, g_fire_next = 0;
static pthread_mutex_t g_fire_mu = PTHREAD_MUTEX_INITIALIZER;
static long long g_fire_last_ms = 0;
static uint32_t g_fire_rng = 2463534242u;
typedef void (*LevelAnimTickFn)(void* level, void* entity);
typedef void (*AddParticleFn)(void* level, int type, const float* pos, const float* vel, int data);
// BlockID dikembalikan lewat penunjuk tersembunyi. Di ARM penunjuk itu ada di r0 dan 'this' (BlockSource) bergeser ke r1:
// r0=penunjuk hasil, r1=region, r2=x, r3=y, stack=z (dibuktikan dari getBlockID(BlockPos const&): mov r4,r0 / mov r6,r1 ... strb r0,[r4])
typedef void (*GetBlockIdXYZFn)(void* ret, void* region, int x, int y, int z);
static LevelAnimTickFn orig_level_anim = nullptr;

static float fire_rand() {   // 0..1
  g_fire_rng ^= g_fire_rng << 13; g_fire_rng ^= g_fire_rng >> 17; g_fire_rng ^= g_fire_rng << 5;
  return (g_fire_rng & 0xFFFFFF) / 16777216.0f;
}

static int g_fire_trace_n = 0;
static void fire_trace(const char* tag, int a, int b, int c) {
  if (g_fire_trace_n >= 60) return;
  g_fire_trace_n++;
  lantern_log("[jejak unggun] %s %d %d %d", tag, a, b, c);
}

static void fire_record(int x, int y, int z) {
  if (x < -30000000 || x > 30000000 || z < -30000000 || z > 30000000 || y < -64 || y > 512) return;
  pthread_mutex_lock(&g_fire_mu);
  bool have = false;
  for (int i = 0; i < g_fire_cnt; i++)
    if (g_fire_pos[i][0] == x && g_fire_pos[i][1] == y && g_fire_pos[i][2] == z) { have = true; break; }
  if (!have) {
    int i = (g_fire_cnt < FIRE_MAX) ? g_fire_cnt++ : (g_fire_next++ % FIRE_MAX);
    g_fire_pos[i][0] = x; g_fire_pos[i][1] = y; g_fire_pos[i][2] = z;
  }
  pthread_mutex_unlock(&g_fire_mu);
}

static void campfire_effects(void* level, void* entity) {
  uint8_t* e = (uint8_t*)entity;
  void* region = *(void**)(e + ENTITY_REGION_OFF);
  if (!region) { fire_trace("effects: region null", 0, 0, 0); return; }
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  long long ms = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
  if (ms - g_fire_last_ms < 50) return;   // paling sering 20 kali per detik
  g_fire_last_ms = ms;
  const float* pp = (const float*)(e + ENTITY_POS_OFF);
  AddParticleFn add = (AddParticleFn)(g_base + OFF_LEVEL_ADD_PARTICLE);
  GetBlockIdXYZFn get_id = (GetBlockIdXYZFn)(g_base + OFF_BS_GET_BLOCK_ID_XYZ);
  pthread_mutex_lock(&g_fire_mu);
  for (int i = 0; i < g_fire_cnt; ) {
    int x = g_fire_pos[i][0], y = g_fire_pos[i][1], z = g_fire_pos[i][2];
    float dx = x + 0.5f - pp[0], dy = y - pp[1], dz = z + 0.5f - pp[2];
    if (dx * dx + dz * dz > FIRE_RANGE * FIRE_RANGE || dy > FIRE_RANGE || dy < -FIRE_RANGE) { i++; continue; }
    alignas(8) uint8_t bid[8] = {0};
    fire_trace("effects: sebelum getBlockID", x, y, z);
    get_id(bid, region, x, y, z);
    fire_trace("effects: sesudah getBlockID id=", bid[0], 0, 0);
    if (bid[0] != g_lt_ids[3]) {   // blok sudah dihancurkan: hapus dari daftar
      g_fire_pos[i][0] = g_fire_pos[g_fire_cnt - 1][0]; g_fire_pos[i][1] = g_fire_pos[g_fire_cnt - 1][1]; g_fire_pos[i][2] = g_fire_pos[g_fire_cnt - 1][2];
      g_fire_cnt--;
      continue;
    }
    if (fire_rand() < 0.35f) {   // asap naik pelan dari tengah tumpukan kayu
      float p[3] = {x + 0.5f + (fire_rand() - 0.5f) * 0.4f, y + 0.55f, z + 0.5f + (fire_rand() - 0.5f) * 0.4f};
      float v[3] = {(fire_rand() - 0.5f) * 0.01f, 0.05f + fire_rand() * 0.03f, (fire_rand() - 0.5f) * 0.01f};
      fire_trace("effects: sebelum asap", x, y, z);
      add(level, PARTICLE_SMOKE, p, v, 0);
      fire_trace("effects: sesudah asap", 0, 0, 0);
    }
    if (g_fire_flame && fire_rand() < 0.5f) {   // percikan api kecil di atas kayu
      float p[3] = {x + 0.5f + (fire_rand() - 0.5f) * 0.5f, y + 0.3f + fire_rand() * 0.15f, z + 0.5f + (fire_rand() - 0.5f) * 0.5f};
      float v[3] = {0.0f, 0.01f, 0.0f};
      fire_trace("effects: sebelum api", x, y, z);
      add(level, PARTICLE_FLAME, p, v, 0);
      fire_trace("effects: sesudah api", 0, 0, 0);
    }
    i++;
  }
  pthread_mutex_unlock(&g_fire_mu);
}

static void hook_level_animate_tick(void* level, void* entity) {
  if (entity) {   // posisi pemain (dipakai pantulan air untuk tinggi kamera)
    const float* pp = (const float*)((uint8_t*)entity + ENTITY_POS_OFF);
    if (pp[1] > -64.0f && pp[1] < 512.0f) { g_pl_x = pp[0]; g_pl_y = pp[1]; g_pl_z = pp[2]; g_pl_frame = g_frames; g_pl_ok = true; }
  }
  orig_level_anim(level, entity);   // sampling animateTick bawaan jalan dulu
  if (g_fire_smoke && g_fire_cnt > 0 && g_lt_ids[3] >= 0 && level && entity) campfire_effects(level, entity);
}

static void install_campfire_fx(uintptr_t base) {
  uintptr_t t = base + OFF_LEVEL_ANIMATE_TICK - 1;
  if (memcmp((void*)t, SIG_LEVEL_ANIMATE_TICK, 8) != 0) { lantern_log("asap api unggun: tanda Level::animateTick tidak cocok, dilewati"); return; }
  bool ok = install_hook(t, (void*)hook_level_animate_tick, (void**)&orig_level_anim);
  lantern_log("asap api unggun: hook Level::animateTick=%d smoke=%d flame=%d", (int)ok, g_fire_smoke, g_fire_flame);
}
// ==============================================================================

static void hook_init_blocks() {
  orig_init_blocks();             // semua blok bawaan sudah terdaftar
  if (lantern_enabled()) register_lantern();
  else lantern_log("lentera nonaktif (pipe3d_lantern.txt tidak ada / enabled=0)");
}
static void hook_bg_init_blocks() {
  orig_bg_init_blocks();          // gambar semua blok bawaan sudah terdaftar
  if (g_lt_ids[0] >= 0 || g_lt_ids[1] >= 0 || g_lt_ids[2] >= 0 || g_lt_ids[3] >= 0) {
    if (g_lt_graphics) register_lantern_graphics();
    ensure_lantern_graphics_slot();
  }
}
static void hook_init_creative() {
  orig_init_creative();           // daftar kreatif bawaan sudah terisi
  if (g_lt_creative) add_lantern_creative();
}

// Seperti install_hook, tapi menyalin copy_len byte awal (harus instruksi utuh dan tidak bergantung posisi).
// Dipakai untuk fungsi yang 8 byte pertamanya memotong instruksi 32-bit.
static bool install_hook_len(uintptr_t target, void* hook, void** orig_out, int copy_len) {
  uint8_t* t = (uint8_t*)target;
  uint8_t* tr = (uint8_t*)mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (tr == (uint8_t*)MAP_FAILED) return false;
  memcpy(tr, t, copy_len);
  int ldr_off = copy_len;                       // genap (Thumb)
  int lit_off = (((ldr_off + 4) & ~3) + 4);     // alamat literal, sejajar 4 byte, tepat 4 byte setelah titik baca PC
  tr[ldr_off] = 0xDF; tr[ldr_off + 1] = 0xF8; tr[ldr_off + 2] = 0x04; tr[ldr_off + 3] = 0xF0;  // ldr.w pc, [pc, #4]
  uint32_t back = (uint32_t)(target + copy_len) | 1;
  memcpy(tr + lit_off, &back, 4);
  __builtin___clear_cache((char*)tr, (char*)tr + lit_off + 4);
  *orig_out = (void*)((uintptr_t)tr | 1);

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

static void install_lantern_patch(uintptr_t base) {
  g_base = base;
  uintptr_t t1 = base + OFF_INIT_BLOCKS - 1;       // alamat di atas punya bit Thumb
  uintptr_t t2 = base + OFF_BG_INIT_BLOCKS - 1;
  uintptr_t t3 = base + OFF_INIT_CREATIVE - 1;
  if (memcmp((void*)t1, SIG_INIT_BLOCKS, 10) != 0 ||
      memcmp((void*)t2, SIG_BG_INIT_BLOCKS, 8) != 0 ||
      memcmp((void*)t3, SIG_INIT_CREATIVE, 8) != 0) {
    LOG("lentera: tanda fungsi tidak cocok, dibatalkan");
    return;
  }
  void** blocks = (void**)(g_base + OFF_MBLOCKS);
  if (blocks[1]) {   // initBlocks sudah selesai sebelum hook terpasang: blok baru tidak aman ditambah sekarang
    lantern_log("initBlocks sudah jalan sebelum hook terpasang; lentera dilewati (tutup game sepenuhnya lalu buka lagi)");
    install_campfire_fx(base);   // tetap pasang hook animateTick (posisi pemain untuk pantulan air)
    return;
  }
  bool ok1 = install_hook_len(t1, (void*)hook_init_blocks, (void**)&orig_init_blocks, 10);
  bool ok2 = install_hook(t2, (void*)hook_bg_init_blocks, (void**)&orig_bg_init_blocks);
  bool ok3 = install_hook(t3, (void*)hook_init_creative, (void**)&orig_init_creative);
  LOG("lentera: hook initBlocks=%d graphics=%d creative=%d", (int)ok1, (int)ok2, (int)ok3);
  install_campfire_fx(base);   // asap api unggun
}
// ==================================================================================

static void* worker(void*) {
  uintptr_t base = 0;
  for (int i = 0; i < 600 && !base; i++) {   // tunggu sampai 60 detik
    dl_iterate_phdr(find_cb, &base);
    if (!base) usleep(100000);
  }
  if (!base) { LOG("libminecraftpe.so tidak ditemukan"); return nullptr; }
  install_gl_spy();   // tahap 1: hanya mencatat, tidak mengubah tampilan
  install_biome_patch(base);   // gunung tinggi (lihat bagian GUNUNG TINGGI)
  install_lantern_patch(base);   // lentera (lihat bagian LENTERA)
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
