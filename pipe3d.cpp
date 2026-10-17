// Meja kerja 3D untuk Minecraft PE 0.15.10 (armeabi-v7a)
// Versi 11: meja kerja (kotak) + tungku ke lapisan transparan + daun bentuk video (dua atap bersilang, dua lapis rapat, bidang penuh 16px miring 45 derajat, warna lebih terang) + log kecil.
// Versi 12: + animasi karakter (napas, jalan/lari halus, kepala halus, sneak halus) lewat hook HumanoidModel::setupAnim. Kode blok dan daun tidak diubah.
#include <android/log.h>
#include <link.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <math.h>
#include <time.h>

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

// ======================= Animasi karakter (HumanoidModel) =======================
// Pendekatan: panggil setupAnim asli, lalu tambah gerakan kecil ke sudut/posisi bagian badan.
// Letak bagian di dalam HumanoidModel (tiap bagian ModelPart besarnya 0xB8 byte):
//   posisi x,y,z di +0,+4,+8 (y positif = ke bawah), sudut putar x,y,z di +0x18,+0x1C,+0x20 (radian)
static const uintptr_t OFF_SETUP_ANIM = 0x910B80;   // HumanoidModel::setupAnim(float x6)
static const uint8_t SIG_SETUP_ANIM[8] = {0x2d, 0xe9, 0xf0, 0x41, 0x2d, 0xed, 0x0a, 0x8b};
static const uintptr_t OFF_PREPARE_MOB = 0x910B6C;  // HumanoidModel::prepareMobModel(Mob&, float x3)
static const uint8_t SIG_PREPARE_MOB[8] = {0x10, 0xb5, 0x04, 0x46, 0x08, 0x68, 0xd0, 0xf8};
static const int P_HEAD = 0xF0, P_HAT = 0x1A8, P_BODY = 0x260;
static const int P_RARM = 0x318, P_LARM = 0x3D0, P_RLEG = 0x488, P_LLEG = 0x540;
static const int M_SNEAK = 0x5F8;    // byte: sedang sneak (diisi prepareMobModel)
static const int M_RIDING = 0x19;    // byte: sedang menunggang
static const int M_ATTACK = 0x14;    // float: progres ayunan serangan

// Angka yang bisa diatur (kalau gerakan terasa kurang/terlalu)
static const float SNEAK_TIME = 0.20f;    // detik untuk masuk/keluar sneak
static const float HEAD_TAU = 0.07f;      // makin besar, kepala makin lambat menyusul
static const float BREATH_SPEED = 1.9f;   // radian per detik (sekitar 3 detik per tarikan napas)
static const float BREATH_AMP = 0.30f;    // naik-turun badan (piksel model)
static const float BREATH_SWAY = 0.035f;  // goyang tangan saat diam (radian)
static const float WALK_BOB = 0.70f;      // badan naik tiap langkah (piksel model)
static const float WALK_TWIST = 0.10f;    // badan memutar tiap langkah (radian); balik tanda kalau arahnya terasa terbalik
static const float RUN_LEAN = 0.12f;      // badan condong ke depan saat lari (radian)

typedef void (*SetupAnimFn)(void* self, float a1, float a2, float a3, float a4, float a5, float a6);
typedef void (*PrepareMobFn)(void* self, void* mob, float a, float b, float c);
static SetupAnimFn orig_setup_anim = nullptr;
static PrepareMobFn orig_prepare_mob = nullptr;
static void* g_cur_mob = nullptr;     // mob yang sedang disiapkan (dicatat prepareMobModel)
static double g_cur_mob_time = 0;
static bool anim_logged = false;

static const int PART_OFFS[7] = {P_HEAD, P_HAT, P_BODY, P_RARM, P_LARM, P_RLEG, P_LLEG};
static float* part(void* self, int off) { return (float*)((uint8_t*)self + off); }
static void snap_pose(void* self, float out[7][6]) {
  for (int i = 0; i < 7; i++) {
    float* p = part(self, PART_OFFS[i]);
    out[i][0] = p[0]; out[i][1] = p[1]; out[i][2] = p[2];
    out[i][3] = p[6]; out[i][4] = p[7]; out[i][5] = p[8];
  }
}
static void put_pose(void* self, const float in[7][6]) {
  for (int i = 0; i < 7; i++) {
    float* p = part(self, PART_OFFS[i]);
    p[0] = in[i][0]; p[1] = in[i][1]; p[2] = in[i][2];
    p[6] = in[i][3]; p[7] = in[i][4]; p[8] = in[i][5];
  }
}

// Keadaan per makhluk (model dipakai bersama oleh banyak mob, jadi tidak boleh disimpan di model)
struct AnimState { void* key; double last; float hy, hp, sneak; bool used; };
static AnimState g_states[32];
static AnimState* get_state(void* key) {
  AnimState* oldest = &g_states[0];
  for (int i = 0; i < 32; i++) {
    if (g_states[i].used && g_states[i].key == key) return &g_states[i];
    if (!g_states[i].used) { oldest = &g_states[i]; break; }
    if (g_states[i].last < oldest->last) oldest = &g_states[i];
  }
  oldest->used = true; oldest->key = key; oldest->last = -1; oldest->hy = oldest->hp = oldest->sneak = 0;
  return oldest;
}
// setupAnim asli TIDAK mereset posisi-y badan/tangan dan sudut-y badan (kecuali saat memukul),
// jadi tambahan dari frame sebelumnya dibatalkan dulu sebelum fungsi asli dipanggil.
struct ModelFx { void* self; float dy, twist; };
static ModelFx g_fx[16];
static ModelFx* get_fx(void* self) {
  for (int i = 0; i < 16; i++) if (g_fx[i].self == self) return &g_fx[i];
  for (int i = 0; i < 16; i++) if (!g_fx[i].self) { g_fx[i].self = self; g_fx[i].dy = g_fx[i].twist = 0; return &g_fx[i]; }
  g_fx[0].self = self; g_fx[0].dy = g_fx[0].twist = 0;
  return &g_fx[0];
}
static double now_sec() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void hook_prepare_mob(void* self, void* mob, float a, float b, float c) {
  g_cur_mob = mob;
  g_cur_mob_time = now_sec();
  orig_prepare_mob(self, mob, a, b, c);
}

static void hook_setup_anim(void* self, float a1, float a2, float a3, float a4, float a5, float a6) {
  double now = now_sec();
  uint8_t* sneak_flag = (uint8_t*)self + M_SNEAK;
  uint8_t real_sneak = *sneak_flag;
  void* key = (g_cur_mob && now - g_cur_mob_time < 0.05) ? g_cur_mob : self;
  AnimState* st = get_state(key);
  bool fresh = (st->last < 0 || now - st->last > 1.0);
  float dt = fresh ? 0.0f : (float)(now - st->last);
  if (dt > 0.1f) dt = 0.1f;
  st->last = now;

  ModelFx* fx = get_fx(self);
  part(self, P_BODY)[1] -= fx->dy;
  part(self, P_RARM)[1] -= fx->dy;
  part(self, P_LARM)[1] -= fx->dy;
  part(self, P_BODY)[7] -= fx->twist;
  fx->dy = 0; fx->twist = 0;

  // 1. Sneak halus: hitung pose tanpa dan dengan sneak, lalu campur sesuai st->sneak
  float target = real_sneak ? 1.0f : 0.0f;
  if (fresh) st->sneak = target;
  else if (st->sneak < target) { st->sneak += dt / SNEAK_TIME; if (st->sneak > target) st->sneak = target; }
  else if (st->sneak > target) { st->sneak -= dt / SNEAK_TIME; if (st->sneak < target) st->sneak = target; }
  float s = st->sneak;
  if (s <= 0.001f || s >= 0.999f) {
    *sneak_flag = (s >= 0.5f) ? 1 : 0;
    orig_setup_anim(self, a1, a2, a3, a4, a5, a6);
  } else {
    float A[7][6], B[7][6], M[7][6];
    *sneak_flag = 0; orig_setup_anim(self, a1, a2, a3, a4, a5, a6); snap_pose(self, A);
    *sneak_flag = 1; orig_setup_anim(self, a1, a2, a3, a4, a5, a6); snap_pose(self, B);
    float e = s * s * (3.0f - 2.0f * s);
    for (int i = 0; i < 7; i++) for (int k = 0; k < 6; k++) M[i][k] = A[i][k] + (B[i][k] - A[i][k]) * e;
    put_pose(self, M);
  }
  *sneak_flag = real_sneak;

  float* head = part(self, P_HEAD);
  float* hat = part(self, P_HAT);
  float* body = part(self, P_BODY);
  float* ra = part(self, P_RARM);
  float* la = part(self, P_LARM);
  bool riding = *((uint8_t*)self + M_RIDING) != 0;
  float attack = *(float*)((uint8_t*)self + M_ATTACK);

  // 2. Kepala menyusul arah pandang dengan halus (x = naik-turun, y = menoleh)
  if (fresh) { st->hy = head[7]; st->hp = head[6]; }
  else {
    float k = 1.0f - expf(-dt / HEAD_TAU);
    st->hy += (head[7] - st->hy) * k;
    st->hp += (head[6] - st->hp) * k;
  }
  head[7] = st->hy; head[6] = st->hp;

  if (!riding) {
    float spd = a2 < 0 ? 0 : (a2 > 1 ? 1 : a2);
    // 3. Napas saat diam: badan, kepala, dan tangan naik-turun pelan; kaki tetap
    float idle = 1.0f - spd * 10.0f;
    if (idle < 0 || attack > 0) idle = 0;
    if (idle > 0) {
      float dy = (float)sin(now * BREATH_SPEED) * BREATH_AMP * idle;
      head[1] += dy; body[1] += dy; ra[1] += dy; la[1] += dy;
      fx->dy += dy;
      float sway = ((float)cos(now * BREATH_SPEED * 0.9) * 0.5f + 0.5f) * BREATH_SWAY * idle;
      ra[8] += sway; la[8] -= sway;
    }
    // 4. Jalan/lari: badan naik tiap langkah, memutar sedikit, condong saat cepat
    float w = spd * (1.0f - 0.5f * st->sneak);
    if (w > 0) {
      float phase = a1 * 0.6662f;
      float bob = fabsf(sinf(phase)) * w * WALK_BOB;
      float twist = (attack > 0) ? 0.0f : sinf(phase) * w * WALK_TWIST;
      float lean = w * RUN_LEAN;
      head[1] -= bob; body[1] -= bob; ra[1] -= bob; la[1] -= bob;
      fx->dy -= bob;
      body[7] += twist; ra[7] += twist; la[7] += twist; head[7] -= twist;
      fx->twist += twist;
      body[6] += lean; ra[6] += lean; la[6] += lean; head[6] -= lean;
    }
  }

  // Topi selalu sama dengan kepala (seperti yang dilakukan setupAnim asli)
  hat[0] = head[0]; hat[1] = head[1]; hat[2] = head[2];
  hat[6] = head[6]; hat[7] = head[7]; hat[8] = head[8];

  if (!anim_logged) {
    anim_logged = true;
    FILE* f = fopen("/sdcard/games/com.mojang/pipe3d_log.txt", "a");
    if (f) {
      fprintf(f, "anim: self=%p sneak=%d riding=%d attack=%f a=%f %f %f %f %f %f head=%f %f body=%f\n",
              self, (int)real_sneak, (int)riding, attack, a1, a2, a3, a4, a5, a6, head[6], head[7], body[1]);
      fclose(f);
    }
  }
}

static void install_anim_hooks(uintptr_t base) {
  uintptr_t t1 = base + OFF_SETUP_ANIM, t2 = base + OFF_PREPARE_MOB;
  if (memcmp((void*)t1, SIG_SETUP_ANIM, 8) != 0 || memcmp((void*)t2, SIG_PREPARE_MOB, 8) != 0) {
    LOG("anim: tanda fungsi tidak cocok, dibatalkan");
    return;
  }
  bool ok2 = install_hook(t2, (void*)hook_prepare_mob, (void**)&orig_prepare_mob);
  bool ok1 = install_hook(t1, (void*)hook_setup_anim, (void**)&orig_setup_anim);
  LOG("anim hook: setupAnim=%d prepareMob=%d", (int)ok1, (int)ok2);
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
  install_anim_hooks(base);
  return nullptr;
}

__attribute__((constructor)) static void init() {
  LOG("pipe3d loaded");
  pthread_t th;
  pthread_create(&th, nullptr, worker, nullptr);
  pthread_detach(th);
}
