// Meja kerja 3D untuk Minecraft PE 0.15.10 (armeabi-v7a)
// Versi 11 + PENGINTAI GL (tahap 1 dan 2 shadow map: hanya mencatat panggilan OpenGL, tidak mengubah tampilan).
// Tahap 2: pada frame 500, 1000, 1500 semua draw terrain dicatat rinci (format vertex, buffer, matriks, origin chunk).
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
#include <elf.h>

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

struct ProgStat { unsigned id; unsigned draws; unsigned verts; unsigned mode; unsigned type; bool described; int kind; };   // kind: 0 belum dicek, 1 terrain, 2 bukan
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
static void* g_glib = nullptr;
static gl_loc_t f_getuloc = nullptr, f_getaloc = nullptr;
static gl_unifv_t f_unifv = nullptr;
static gl_unifiv_t f_unifiv = nullptr;
static gl_vaiv_t f_vaiv = nullptr;
static gl_vaptr_t f_vaptr = nullptr;
static gl_isen_t f_isen = nullptr;
static gl_getb_t f_getb = nullptr;
static gl_geti_t f_geti = nullptr;
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
  if (n >= 80) return;
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

static void spy_use_program(GLuint_t p) {
  g_cur_prog = p;
  if (p) {
    ProgStat* s = prog_stat(p);
    if (s->kind == 0) {   // program terrain = yang punya uniform CHUNK_ORIGIN_AND_SCALE
      cap_init();
      int loc = f_getuloc ? f_getuloc(p, "CHUNK_ORIGIN_AND_SCALE") : -1;
      s->kind = loc >= 0 ? 1 : 2;
    }
  }
  o_useprog(p);
}
static void spy_draw_elements(unsigned mode, int count, unsigned type, const void* idx) {
  ProgStat* s = prog_stat(g_cur_prog);
  s->draws++; s->verts += (unsigned)count; s->mode = mode; s->type = type;
  if (g_capturing && s->kind == 1) capture_draw(s, mode, count, type, idx);
  o_drawel(mode, count, type, idx);
}
static void spy_draw_arrays(unsigned mode, int first, int count) {
  ProgStat* s = prog_stat(g_cur_prog);
  s->draws++; s->verts += (unsigned)count; s->mode = mode; s->type = 0;
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
  if (g_frames == 500 || g_frames == 1000 || g_frames == 1500) {
    g_cap_f = fopen("/sdcard/games/com.mojang/pipe3d_log.txt", "a");
    if (g_cap_f) { g_capturing = true; g_cap_draws = 0; g_cap_mat = false; fprintf(g_cap_f, "T2 mulai frame=%u\n", g_frames); }
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
