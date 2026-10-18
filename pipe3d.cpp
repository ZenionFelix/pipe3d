// Meja kerja 3D untuk Minecraft PE 0.15.10 (armeabi-v7a)
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
  int loc_origin, loc_wv, loc_proj, loc_wvp, loc_cc, loc_vp, loc_t2; float vp_orig[2]; int vp_state; };   // kind: 0 belum dicek, 1 terrain, 2 bukan
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
  X(void, Uniform2f, (int, float, float)) \
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
  X(void, GetBufferParameteriv, (unsigned, unsigned, int*))

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
static int g_cfg_on = 1, g_cfg_size = 1024, g_cfg_capture = 0, g_cfg_level = -1;
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
    else if (sscanf(line, "level=%d", &iv) == 1) g_cfg_level = (iv < 0) ? 0 : (iv > 3 ? 3 : iv);
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
// level 0: mati; 1: hanya mengenali arah matahari; 2: + menggambar peta bayangan (belum dipakai shader); 3: penuh.
// Pada frame terrain pertama, file status ditandai "armed=1". Kalau game crash atau macet sebelum 90 frame terrain berlalu,
// peluncuran berikutnya otomatis turun satu level. Untuk memaksa level tertentu: tulis "level=N" di pipe3d_shadow.txt.
static const char* GUARD_FILE = "/sdcard/games/com.mojang/pipe3d_state.txt";
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
  int lvl = 3, armed = 0; bool have = false;
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
  else if (have && armed) { sh_log("pipe3d: peluncuran sebelumnya tidak lolos 90 frame terrain pada level %d, turun ke level %d\n", lvl, lvl > 0 ? lvl - 1 : 0); lvl = lvl > 0 ? lvl - 1 : 0; }
  if (lvl < 0) lvl = 0;
  if (lvl > 3) lvl = 3;
  if (!g_cfg_on) lvl = 0;
  g_level = lvl;
  guard_write(g_level, 0);
  sh_log("pipe3d: bayangan level=%d\n", g_level);
}
static void guard_arm() {
  g_guard_done = true;
  if (g_level >= 1) { guard_write(g_level, 1); g_guard_armed = true; }
  bc("frame terrain pertama, pengaman aktif");
}

// ---- sumber daya peta bayangan ----
static int g_sh_state = 0;   // 0 belum diinisialisasi, 1 siap, -1 gagal/nonaktif
static unsigned g_sh_tex = 0, g_sh_fbo = 0, g_sh_prog = 0, g_sh_ctex = 0;
static int g_sh_loc_lvp = -1, g_sh_loc_org = -1, g_sh_loc_tex = -1;
static int g_sh_size = 1024;
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
  clear_gl_errors();
  g_sh_state = 1;
  sh_log("shadow: siap. ukuran=%d radius=%.0f bias=%.2f\n", g_sh_size, g_cfg_radius, g_cfg_bias);
}

// ---- status per frame ----
struct DrawRec { int vbo, ibo, count, vsz, isz; unsigned type; uintptr_t idx; float o[3]; };
static DrawRec g_dl[2][2048];
static int g_dn[2] = {0, 0};
static int g_dcur = 0;
static unsigned g_tframe = 0xFFFFFFFFu;   // nomor frame terakhir yang punya draw terrain
static bool g_delta_ok = false;
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

// dipanggil sebelum tiap draw terrain
static int g_u2_prev = 0, g_act_prev = 0;
// hasil true: slot tekstur 2 ditimpa dengan peta bayangan, panggil terrain_draw_post sesudah draw asli
static bool terrain_draw(ProgStat* s, int count, unsigned type, const void* idx) {
  if (!g_level_inited) level_init();
  if (g_level < 1 || g_cur_fbo != 0) return false;
  if (g_tframe != g_frames) {   // draw terrain pertama di frame ini
    g_tframe = g_frames;
    g_delta_ok = false; g_view_ok = false;
    if (!g_guard_done) guard_arm();
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
    if (!g_delta_ok && g_dn[prev] > 0) {   // chunk yang sama di frame sebelumnya -> selisih posisi kamera
      for (int i = 0; i < g_dn[prev]; i++) {
        const DrawRec& r = g_dl[prev][i];
        if (r.vbo == vbo && r.ibo == ibo && r.count == count && r.idx == (uintptr_t)idx) {
          g_delta[0] = r.o[0] - o[0]; g_delta[1] = r.o[1] - o[1]; g_delta[2] = r.o[2] - o[2];
          g_cam_est[0] += g_delta[0]; g_cam_est[1] += g_delta[1]; g_cam_est[2] += g_delta[2];
          g_delta_ok = true; g_delta_ver++;
          break;
        }
      }
    }
    if (g_dn[g_dcur] < 2048 && count > 0 && vbo > 0 && ibo > 0) {
      int vsz = 0, isz = 0;
      gx.GetBufferParameteriv(0x8892, 0x8764, &vsz);   // GL_BUFFER_SIZE
      gx.GetBufferParameteriv(0x8893, 0x8764, &isz);
      DrawRec& r = g_dl[g_dcur][g_dn[g_dcur]++];
      r.vbo = vbo; r.ibo = ibo; r.count = count; r.vsz = vsz; r.isz = isz; r.type = type; r.idx = (uintptr_t)idx;
      r.o[0] = o[0]; r.o[1] = o[1]; r.o[2] = o[2];
    }
  }
  if (g_level < 3) return false;
  bool want = g_lvp_valid && g_delta_ok && sun_enabled();
  if (!want) {
    if (s->vp_state == 1 && s->loc_vp >= 0) { gx.Uniform2f(s->loc_vp, s->vp_orig[0], s->vp_orig[1]); s->vp_state = 0; }
    return false;
  }
  if (s->loc_vp < 0 || s->loc_cc < 0 || s->loc_t2 < 0) return false;   // shader belum memuat jalur bayangan
  static bool first_set = true;
  if (first_set) { first_set = false; bc("parameter bayangan pertama diisi lewat uniform bawaan"); }
  if (s->vp_state == 0) f_unifv(s->id, s->loc_vp, s->vp_orig);   // simpan nilai asli supaya bisa dipulihkan
  // pergeseran kisi: peta dibuat di koordinat kamera frame sebelumnya, jadi tambahkan selisih kamera (g_delta)
  float ox = g_ox0 + g_bs[0] * g_delta[0] + g_bs[1] * g_delta[1] + g_bs[2] * g_delta[2];
  float oy = g_oy0 + g_bu[0] * g_delta[0] + g_bu[1] * g_delta[1] + g_bu[2] * g_delta[2];
  float oz = g_bf[0] * g_delta[0] + g_bf[1] * g_delta[1] + g_bf[2] * g_delta[2];
  float cc[4] = {0, 0, 0, 1};
  f_unifv(s->id, s->loc_cc, cc);                       // .r milik game (efek fade chunk) tetap dipertahankan
  gx.Uniform4f(s->loc_cc, cc[0], ox, oy, oz);
  gx.Uniform2f(s->loc_vp, -(g_gen_Ly + 2.0f), g_gen_Lz);
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
  if (g_delta_ok) g_st_delta_ok++;
  shadow_pass();
  g_dcur = 1 - g_dcur;
  g_dn[g_dcur] = 0;
  if (g_frames % 600 == 0 && g_st_logs < 12) {
    g_st_logs++;
    sh_log("shadow frame %u: pass=%u draw/pass=%.0f lewati/pass=%.0f ms/pass=%.2f delta_ok=%u/%u matahari=(%.3f %.3f %.3f) valid=%d lvp=%d\n",
           g_frames, g_st_pass, g_st_pass ? (double)g_st_draws / g_st_pass : 0.0, g_st_pass ? (double)g_st_skipped / g_st_pass : 0.0,
           g_st_pass ? g_st_ms / g_st_pass : 0.0, g_st_delta_ok, g_st_frames, g_sun[0], g_sun[1], g_sun[2],
           (int)g_sun_valid, (int)g_lvp_valid);
    g_st_pass = g_st_draws = g_st_skipped = g_st_delta_ok = g_st_frames = 0; g_st_ms = 0;
  }
}

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
      s->loc_vp = f_getuloc ? f_getuloc(p, "VIEWPORT_DIMENSION") : -1;
      s->loc_t2 = f_getuloc ? f_getuloc(p, "TEXTURE_2") : -1;
      s->vp_state = 0;
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
  o_drawel(mode, count, type, idx);
  if (u2) terrain_draw_post();
}
static void spy_draw_arrays(unsigned mode, int first, int count) {
  ProgStat* s = prog_stat(g_cur_prog);
  s->draws++; s->verts += (unsigned)count; s->mode = mode; s->type = 0;
  if (g_capturing) capture_other(s, mode, count, s->kind != 1);
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

static void* worker(void*) {
  uintptr_t base = 0;
  for (int i = 0; i < 600 && !base; i++) {   // tunggu sampai 60 detik
    dl_iterate_phdr(find_cb, &base);
    if (!base) usleep(100000);
  }
  if (!base) { LOG("libminecraftpe.so tidak ditemukan"); return nullptr; }
  install_gl_spy();   // tahap 1: hanya mencatat, tidak mengubah tampilan
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
