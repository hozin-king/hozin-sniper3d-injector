// Hozin Sniper 3D Injector — payload (jalan di dalam proses game).
//
// Arsitektur: SEMUA class/method/field IL2CPP di-resolve runtime
// berdasarkan NAMA (il2cpp_class_from_name / get_method_from_name /
// get_field_from_name). TIDAK ADA offset hardcoded — bila game update
// dan sebuah nama hilang, resolve gagal dan payload melaporkan
// PARTIAL/UNSUPPORTED ke file status (fitur yang gagal nonaktif, bukan
// toggle palsu), bukan crash.
//
// Gate mode misi (fail-closed, terpusat di gate_mission()):
//   LevelController jalan DAN ArenaIngameController /
//   ArenaMatchFluxController TIDAK jalan. Bila tak bisa dipastikan,
//   semua fitur MATI.
//
// Aimbot: hook Player.Weapon.ShootTest(Ray, LayerMask) pada class
// konkret senjata pemain (di-resolve dari instance _weapon milik
// CharacterShooter — override subclass ikut ter-hook; bila pemain
// ganti senjata ke subclass lain, hook dipasang ulang). Hook menulis
// ulang ray (origin = posisi kamera, direction = ke kepala target)
// lalu memanggil original. Target = musuh hidup terdekat dari tengah
// layar, dihitung worker thread tiap 100 ms dan di-cache.
#include "il2cpp_api.h"
#include "sniper3d.h"

#include <algorithm>
#include <android/log.h>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <link.h>
#include <map>
#include <mutex>
#include <pthread.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "dobby.h"

#define LOG_TAG "HozinSniper3D"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace sniper3d {
namespace {

constexpr const char* kGameCache = "/data/data/com.fungames.sniper3d/cache";
constexpr const char* kConfigPath = "/data/data/com.fungames.sniper3d/cache/hozinsniper_config.json";
constexpr const char* kEspPath = "/data/data/com.fungames.sniper3d/cache/hozinsniper_esp.json";
constexpr const char* kStatusCache = "/data/data/com.fungames.sniper3d/cache/hozinsniper_payload.log";
constexpr const char* kStatusTmp = "/data/local/tmp/hozinsniper_payload.log";

// ---------------------------------------------------------------- state
struct Config {
    bool aimbot = false;
    bool zoomAim = false;
    bool esp = false;
    bool noSpread = false;
    bool noReload = false;
    bool zoomFar = false;
};

enum class Mode { UNKNOWN, MISI, ARENA };

struct CachedTarget {
    bool valid = false;
    Vector3 headPos{0, 0, 0};
    Vector3 camPos{0, 0, 0};
    long long timestampMs = 0;
};

struct WeaponDataOrig {
    float spread = 0;
    float reloadTime = 0;
    float maxZoom = 0;
    bool saved = false;
};

struct State {
    // libil2cpp
    void* libHandle = nullptr;
    Fn_domain_get domain_get = nullptr;
    Fn_thread_attach thread_attach = nullptr;
    Fn_domain_get_assemblies domain_get_assemblies = nullptr;
    Fn_assembly_get_image assembly_get_image = nullptr;
    Fn_class_from_name class_from_name = nullptr;
    Fn_class_get_method_from_name class_get_method_from_name = nullptr;
    Fn_class_get_field_from_name class_get_field_from_name = nullptr;
    Fn_class_get_parent class_get_parent = nullptr;
    Fn_class_get_type class_get_type = nullptr;
    Fn_type_get_object type_get_object = nullptr;
    Fn_runtime_invoke runtime_invoke = nullptr;
    Fn_object_unbox object_unbox = nullptr;
    Fn_object_get_class object_get_class = nullptr;
    Fn_field_get_offset field_get_offset = nullptr;
    Fn_field_get_value field_get_value = nullptr;
    Fn_field_set_value field_set_value = nullptr;
    Fn_field_static_get_value field_static_get_value = nullptr;
    Fn_field_static_set_value field_static_set_value = nullptr;
    Fn_array_length array_length = nullptr;

    // classes
    Il2CppClass* personClass = nullptr;
    Il2CppClass* weaponClass = nullptr;
    Il2CppClass* shooterClass = nullptr;
    Il2CppClass* weaponDataClass = nullptr;
    Il2CppClass* zoomClass = nullptr;
    Il2CppClass* levelClass = nullptr;
    Il2CppClass* arenaClass = nullptr;
    Il2CppClass* arenaFluxClass = nullptr;
    Il2CppClass* cameraClass = nullptr;
    Il2CppClass* transformClass = nullptr;
    Il2CppClass* objectClass = nullptr;
    Il2CppClass* screenClass = nullptr;
    Il2CppClass* componentClass = nullptr;

    // methods
    Il2CppMethod* cameraGetMain = nullptr;
    Il2CppMethod* cameraWorldToScreen = nullptr;
    Il2CppMethod* transformGetPosition = nullptr;
    Il2CppMethod* componentGetTransform = nullptr;
    Il2CppMethod* objectFindObjects = nullptr;      // FindObjectsOfType(Type)
    Il2CppMethod* objectFindObjectsByType = nullptr;  // fallback Unity 6
    Il2CppMethod* screenGetWidth = nullptr;
    Il2CppMethod* screenGetHeight = nullptr;
    Il2CppMethod* zoomGetIsZoomingIn = nullptr;
    Il2CppMethod* personGetBaseHealth = nullptr;
    Il2CppMethod* shooterGetCurrentAmmo = nullptr;
    Il2CppMethod* shooterSetCurrentAmmo = nullptr;
    Il2CppMethod* dataGetSpread = nullptr;
    Il2CppMethod* dataSetSpread = nullptr;
    Il2CppMethod* dataGetReloadTime = nullptr;
    Il2CppMethod* dataSetReloadTime = nullptr;
    Il2CppMethod* dataGetMaxZoom = nullptr;
    Il2CppMethod* dataSetMaxZoom = nullptr;
    Il2CppMethod* dataGetClipSize = nullptr;

    // fields
    Il2CppField* personCurrentHealth = nullptr;
    Il2CppField* personAlive = nullptr;
    Il2CppField* personDead = nullptr;
    Il2CppField* personHitTransform = nullptr;
    Il2CppField* weaponSpreadAmount = nullptr;
    Il2CppField* weaponDataField = nullptr;      // Weapon._data
    Il2CppField* shooterWeapon = nullptr;        // CharacterShooter._weapon
    Il2CppField* shooterWeaponData = nullptr;    // CharacterShooter._weaponData
    Il2CppField* zoomScaleMax = nullptr;         // static float

    bool coreResolved = false;
    bool hookInstalled = false;
    std::vector<void*> hookedMethods;
    std::vector<std::string> failed;
    Mode mode = Mode::UNKNOWN;
    Config config;
    std::mutex targetMutex;
    CachedTarget target;
    float zoomScaleMaxOrig = 0;
    bool zoomScaleMaxSaved = false;
    std::map<void*, WeaponDataOrig> weaponDataOrig;
    long long lastConfigReadMs = 0;
    long long lastModeCheckMs = 0;
    long long lastHookCheckMs = 0;
    long long lastEspWriteMs = 0;
    long long lastStatusWriteMs = 0;
    std::string lastStatus;
};

State g;

long long now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// ---------------------------------------------------------------- files
bool write_file(const char* path, const std::string& content) {
    FILE* f = std::fopen(path, "w");
    if (!f) return false;
    std::fwrite(content.data(), 1, content.size(), f);
    std::fclose(f);
    return true;
}

std::string read_file(const char* path) {
    FILE* f = std::fopen(path, "r");
    if (!f) return "";
    char buf[4096];
    size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    buf[n] = '\0';
    return std::string(buf, n);
}

void write_status(const std::string& status) {
    std::string content = "STATUS=" + status + "\n";
    content += "FAILED=";
    if (g.failed.empty()) {
        content += "-\n";
    } else {
        for (size_t i = 0; i < g.failed.size(); i++) {
            if (i) content += ",";
            content += g.failed[i];
        }
        content += "\n";
    }
    content += "LIBSIZE=";
    // Ukuran file payload sendiri (diagnosis) via dladdr + stat.
    {
        Dl_info info{};
        struct stat st {};
        if (dladdr(reinterpret_cast<void*>(&start_worker), &info) && info.dli_fname &&
            stat(info.dli_fname, &st) == 0) {
            content += std::to_string((long long)st.st_size);
        } else {
            content += "-";
        }
    }
    content += "\n";
    content += "MODE=";
    content += (g.mode == Mode::MISI ? "MISI" : g.mode == Mode::ARENA ? "ARENA" : "UNKNOWN");
    content += "\n";
    content += "HOOK=";
    content += (g.hookInstalled ? "ok" : "gagal");
    content += "\n";
    g.lastStatus = status;
    write_file(kStatusCache, content);
    write_file(kStatusTmp, content);
    g.lastStatusWriteMs = now_ms();
}

void crash_handler(int sig) {
    // Async-signal-safe: open/write/close langsung (tanpa stdio).
    const char* msg = "STATUS=CRASH\nFAILED=signal\nLIBSIZE=-\nMODE=UNKNOWN\nHOOK=gagal\n";
    int fd = open(kStatusCache, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        (void)write(fd, msg, std::strlen(msg));
        close(fd);
    }
    fd = open(kStatusTmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        (void)write(fd, msg, std::strlen(msg));
        close(fd);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

// ---------------------------------------------------------------- config
bool config_flag(const std::string& json, const char* key) {
    std::string pat = std::string("\"") + key + "\":true";
    return json.find(pat) != std::string::npos;
}

void read_config() {
    std::string json = read_file(kConfigPath);
    if (json.empty()) return;
    g.config.aimbot = config_flag(json, "aimbot");
    g.config.zoomAim = config_flag(json, "zoomaim");
    g.config.esp = config_flag(json, "esp");
    g.config.noSpread = config_flag(json, "nospread");
    g.config.noReload = config_flag(json, "noreload");
    g.config.zoomFar = config_flag(json, "zoom");
}

// ---------------------------------------------------------------- il2cpp
bool load_il2cpp_api() {
    g.libHandle = dlopen("libil2cpp.so", RTLD_NOW | RTLD_NOLOAD);
    if (!g.libHandle) {
        // Cari path lengkap di maps lalu dlopen path itu.
        FILE* f = std::fopen("/proc/self/maps", "r");
        if (f) {
            char line[1024];
            while (std::fgets(line, sizeof(line), f)) {
                if (std::strstr(line, "libil2cpp.so")) {
                    char* p = std::strchr(line, '/');
                    if (p) {
                        std::string path(p);
                        while (!path.empty() && (path.back() == '\n' || path.back() == ' ')) path.pop_back();
                        g.libHandle = dlopen(path.c_str(), RTLD_NOW | RTLD_NOLOAD);
                        if (g.libHandle) break;
                    }
                }
            }
            std::fclose(f);
        }
    }
    if (!g.libHandle) {
        g.failed.push_back("libil2cpp.so");
        return false;
    }
#define SYM(name) \
    g.name = reinterpret_cast<Fn_##name>(dlsym(g.libHandle, "il2cpp_" #name)); \
    if (!g.name) g.failed.push_back("il2cpp_" #name);
    SYM(domain_get)
    SYM(thread_attach)
    SYM(domain_get_assemblies)
    SYM(assembly_get_image)
    SYM(class_from_name)
    SYM(class_get_method_from_name)
    SYM(class_get_field_from_name)
    SYM(class_get_parent)
    SYM(class_get_type)
    SYM(type_get_object)
    SYM(runtime_invoke)
    SYM(object_unbox)
    SYM(object_get_class)
    SYM(field_get_offset)
    SYM(field_get_value)
    SYM(field_set_value)
    SYM(field_static_get_value)
    SYM(field_static_set_value)
    SYM(array_length)
#undef SYM
    return g.domain_get && g.thread_attach && g.domain_get_assemblies &&
           g.assembly_get_image && g.class_from_name && g.class_get_method_from_name &&
           g.class_get_field_from_name && g.runtime_invoke && g.object_unbox &&
           g.object_get_class && g.field_get_value && g.field_set_value &&
           g.array_length && g.type_get_object && g.class_get_type;
}

// Cari class di semua assembly (Assembly-CSharp + UnityEngine.*).
Il2CppClass* find_class(const char* namespaze, const char* name) {
    Il2CppDomain* domain = g.domain_get();
    if (!domain) return nullptr;
    size_t count = 0;
    const Il2CppAssembly** assemblies = g.domain_get_assemblies(domain, &count);
    if (!assemblies) return nullptr;
    for (size_t i = 0; i < count; i++) {
        Il2CppImage* image = g.assembly_get_image(assemblies[i]);
        if (!image) continue;
        Il2CppClass* klass = g.class_from_name(image, namespaze, name);
        if (klass) return klass;
    }
    return nullptr;
}

// Cari method, jalan menelusuri parent (override di subclass dicari dulu).
Il2CppMethod* find_method(Il2CppClass* klass, const char* name, int argc) {
    Il2CppClass* cur = klass;
    for (int depth = 0; cur && depth < 8; depth++) {
        Il2CppMethod* m = g.class_get_method_from_name(cur, name, argc);
        if (m) return m;
        cur = g.class_get_parent ? g.class_get_parent(cur) : nullptr;
    }
    return nullptr;
}

Il2CppObject* invoke_obj(Il2CppMethod* method, void* obj, void** params) {
    if (!method) return nullptr;
    void* exc = nullptr;
    Il2CppObject* ret = g.runtime_invoke(method, obj, params, &exc);
    if (exc) return nullptr;
    return ret;
}

template <typename T>
bool invoke_value(Il2CppMethod* method, void* obj, void** params, T& out) {
    Il2CppObject* boxed = invoke_obj(method, obj, params);
    if (!boxed) return false;
    void* unboxed = g.object_unbox(boxed);
    if (!unboxed) return false;
    out = *reinterpret_cast<T*>(unboxed);
    return true;
}

template <typename T>
bool field_get(Il2CppObject* obj, Il2CppField* field, T& out) {
    if (!obj || !field) return false;
    g.field_get_value(obj, field, &out);
    return true;
}

template <typename T>
bool field_set(Il2CppObject* obj, Il2CppField* field, const T& val) {
    if (!obj || !field) return false;
    T v = val;
    g.field_set_value(obj, field, &v);
    return true;
}

// ---------------------------------------------------------------- resolve
bool resolve_all() {
    g.failed.clear();
    if (!g.libHandle && !load_il2cpp_api()) return false;
    if (!g.libHandle) return false;
    Il2CppDomain* domain = g.domain_get();
    if (!domain) return false;
    g.thread_attach(domain);

    auto needClass = [&](Il2CppClass*& slot, const char* ns, const char* name) {
        slot = find_class(ns, name);
        if (!slot) g.failed.push_back(std::string("class:") + ns + "." + name);
    };
    needClass(g.personClass, "Person", "Person");
    needClass(g.weaponClass, "Player", "Weapon");
    needClass(g.shooterClass, "Player", "CharacterShooter");
    needClass(g.weaponDataClass, "", "WeaponData");
    needClass(g.zoomClass, "InputUtils", "Zoom");
    needClass(g.levelClass, "", "LevelController");
    needClass(g.arenaClass, "", "ArenaIngameController");
    needClass(g.arenaFluxClass, "Game.Multiplayer", "ArenaMatchFluxController");
    needClass(g.cameraClass, "UnityEngine", "Camera");
    needClass(g.transformClass, "UnityEngine", "Transform");
    needClass(g.objectClass, "UnityEngine", "Object");
    needClass(g.screenClass, "UnityEngine", "Screen");
    needClass(g.componentClass, "UnityEngine", "Component");

    auto needMethod = [&](Il2CppMethod*& slot, Il2CppClass* klass, const char* name, int argc) {
        if (!klass) return;
        slot = find_method(klass, name, argc);
        if (!slot) g.failed.push_back(std::string("method:") + name);
    };
    needMethod(g.cameraGetMain, g.cameraClass, "get_main", 0);
    needMethod(g.cameraWorldToScreen, g.cameraClass, "WorldToScreenPoint", 1);
    needMethod(g.transformGetPosition, g.transformClass, "get_position", 0);
    needMethod(g.componentGetTransform, g.componentClass, "get_transform", 0);
    needMethod(g.objectFindObjects, g.objectClass, "FindObjectsOfType", 1);
    if (!g.objectFindObjects) {
        needMethod(g.objectFindObjectsByType, g.objectClass, "FindObjectsByType", 2);
        if (g.objectFindObjectsByType) {
            // Fallback Unity 6 berhasil — hapus entri gagal FindObjectsOfType
            // supaya status tidak jadi PARTIAL padahal semua jalan.
            g.failed.erase(std::remove(g.failed.begin(), g.failed.end(),
                                       "method:FindObjectsOfType"),
                           g.failed.end());
        }
    }
    needMethod(g.screenGetWidth, g.screenClass, "get_width", 0);
    needMethod(g.screenGetHeight, g.screenClass, "get_height", 0);
    needMethod(g.zoomGetIsZoomingIn, g.zoomClass, "get_IsZoomingIn", 0);
    needMethod(g.personGetBaseHealth, g.personClass, "get_BaseHealth", 0);
    needMethod(g.shooterGetCurrentAmmo, g.shooterClass, "get_CurrentAmmo", 0);
    needMethod(g.shooterSetCurrentAmmo, g.shooterClass, "set_CurrentAmmo", 1);
    needMethod(g.dataGetSpread, g.weaponDataClass, "get_Spread", 0);
    needMethod(g.dataSetSpread, g.weaponDataClass, "set_Spread", 1);
    needMethod(g.dataGetReloadTime, g.weaponDataClass, "get_ReloadTime", 0);
    needMethod(g.dataSetReloadTime, g.weaponDataClass, "set_ReloadTime", 1);
    needMethod(g.dataGetMaxZoom, g.weaponDataClass, "get_MaxZoom", 0);
    needMethod(g.dataSetMaxZoom, g.weaponDataClass, "set_MaxZoom", 1);
    needMethod(g.dataGetClipSize, g.weaponDataClass, "get_ClipSize", 0);

    auto needField = [&](Il2CppField*& slot, Il2CppClass* klass, const char* name) {
        if (!klass) return;
        slot = g.class_get_field_from_name(klass, name);
        if (!slot) g.failed.push_back(std::string("field:") + name);
    };
    needField(g.personCurrentHealth, g.personClass, "_currentHealth");
    needField(g.personAlive, g.personClass, "_alive");
    needField(g.personDead, g.personClass, "_dead");
    needField(g.personHitTransform, g.personClass, "_hitTransform");
    needField(g.weaponSpreadAmount, g.weaponClass, "_spreadAmount");
    needField(g.weaponDataField, g.weaponClass, "_data");
    needField(g.shooterWeapon, g.shooterClass, "_weapon");
    needField(g.shooterWeaponData, g.shooterClass, "_weaponData");
    needField(g.zoomScaleMax, g.zoomClass, "ScaleMax");

    bool core = g.personClass && g.weaponClass && g.shooterClass && g.weaponDataClass &&
                g.cameraClass && g.transformClass && g.objectClass && g.componentClass &&
                (g.objectFindObjects || g.objectFindObjectsByType) &&
                g.cameraGetMain && g.cameraWorldToScreen && g.transformGetPosition &&
                g.componentGetTransform && g.personCurrentHealth && g.personAlive &&
                g.personDead && g.personHitTransform && g.weaponSpreadAmount &&
                g.weaponDataField && g.shooterWeapon && g.shooterWeaponData;
    g.coreResolved = core;
    return core;
}

// ---------------------------------------------------------------- invoke helpers
std::vector<Il2CppObject*> find_objects(Il2CppClass* klass) {
    std::vector<Il2CppObject*> out;
    if (!klass) return out;
    Il2CppType* type = g.class_get_type(klass);
    if (!type) return out;
    Il2CppObject* sysType = g.type_get_object(type);
    if (!sysType) return out;
    Il2CppObject* arrObj = nullptr;
    if (g.objectFindObjects) {
        void* params[1] = {&sysType};
        arrObj = invoke_obj(g.objectFindObjects, nullptr, params);
    } else if (g.objectFindObjectsByType) {
        int sortMode = 0;
        void* params[2] = {&sysType, &sortMode};
        arrObj = invoke_obj(g.objectFindObjectsByType, nullptr, params);
    }
    if (!arrObj) return out;
    uint32_t len = g.array_length(arrObj);
    Il2CppObject** items = reinterpret_cast<Il2CppObject**>(
        reinterpret_cast<char*>(arrObj) + sizeof(Il2CppArrayHeader));
    for (uint32_t i = 0; i < len; i++) {
        if (items[i]) out.push_back(items[i]);
    }
    return out;
}

Il2CppObject* get_main_camera() {
    return invoke_obj(g.cameraGetMain, nullptr, nullptr);
}

bool get_transform(Il2CppObject* obj, Il2CppObject*& transformOut) {
    Il2CppObject* t = invoke_obj(g.componentGetTransform, obj, nullptr);
    if (!t) return false;
    transformOut = t;
    return true;
}

bool get_position(Il2CppObject* transform, Vector3& pos) {
    return invoke_value<Vector3>(g.transformGetPosition, transform, nullptr, pos);
}

bool world_to_screen(Il2CppObject* camera, const Vector3& world, Vector3& screen) {
    Vector3 w = world;
    void* params[1] = {&w};
    return invoke_value<Vector3>(g.cameraWorldToScreen, camera, params, screen);
}

int screen_width() {
    int w = 0;
    invoke_value<int>(g.screenGetWidth, nullptr, nullptr, w);
    return w;
}

int screen_height() {
    int h = 0;
    invoke_value<int>(g.screenGetHeight, nullptr, nullptr, h);
    return h;
}

// ---------------------------------------------------------------- gate
// SATU gate terpusat: misi HANYA bila LevelController jalan DAN kedua
// controller Arena TIDAK jalan. Bila ada class gate yang tak ter-resolve
// → UNKNOWN → semua fitur MATI.
void update_mode() {
    if (!g.levelClass || !g.arenaClass || !g.arenaFluxClass) {
        g.mode = Mode::UNKNOWN;
        return;
    }
    bool level = !find_objects(g.levelClass).empty();
    bool arena = !find_objects(g.arenaClass).empty();
    bool flux = !find_objects(g.arenaFluxClass).empty();
    if (level && !arena && !flux) {
        g.mode = Mode::MISI;
    } else if (arena || flux) {
        g.mode = Mode::ARENA;
    } else {
        g.mode = Mode::UNKNOWN;
    }
}

// ---------------------------------------------------------------- ESP + target
struct EspEntry {
    float x, y, w, h, hp, dist;
    std::string name;
};

std::vector<EspEntry> g_espEntries;
bool g_isZooming = false;

void enumerate_and_target() {
    g_espEntries.clear();
    Il2CppObject* camera = get_main_camera();
    if (!camera) return;
    int sw = screen_width();
    int sh = screen_height();
    if (sw <= 0 || sh <= 0) return;

    auto persons = find_objects(g.personClass);
    float bestScore = 1e9f;
    CachedTarget best;
    for (Il2CppObject* person : persons) {
        // MITIGASI PARSIAL (v1.0.3): enumerasi + baca field masih jalan
        // dari worker thread (API Unity resmi main-thread-only) — baca
        // objek yang sudah di-destroy antara enumerasi dan baca bisa
        // SIGSEGV. Cek class dulu: menangkap objek yang memorinya sudah
        // dipakai ulang / di-nol-kan. BUKAN perbaikan penuh: pindah
        // enumerasi ke main thread butuh hook per-frame terverifikasi
        // (belum ada titik hook yang aman dari REA) — jangan klaim penuh.
        if (!g.object_get_class(person)) continue;
        bool alive = false, dead = false;
        double health = 0;
        field_get(person, g.personAlive, alive);
        field_get(person, g.personDead, dead);
        field_get(person, g.personCurrentHealth, health);
        if (!alive || dead || health <= 0) continue;

        Il2CppObject* transform = nullptr;
        if (!get_transform(person, transform)) continue;
        Vector3 pos;
        if (!get_position(transform, pos)) continue;

        // Kepala: _hitTransform bila ada, kalau tidak posisi + 1,5 m.
        Vector3 headPos = pos;
        Il2CppObject* hitTransform = nullptr;
        field_get(person, g.personHitTransform, hitTransform);
        if (hitTransform) {
            Vector3 hp;
            if (get_position(hitTransform, hp)) headPos = hp;
        } else {
            headPos.y += 1.5f;
        }

        Vector3 scr;
        if (!world_to_screen(camera, headPos, scr)) continue;
        if (scr.z <= 0) continue;  // di belakang kamera
        float sx = scr.x;
        float sy = (float)sh - scr.y;
        if (sx < 0 || sx > sw || sy < 0 || sy > sh) continue;

        double baseHealth = 0;
        invoke_value<double>(g.personGetBaseHealth, person, nullptr, baseHealth);
        float hpRatio = baseHealth > 0 ? (float)(health / baseHealth) : 0.0f;
        float dist = scr.z;
        float boxH = dist > 0.5f ? (1.5f * (float)sh / dist) : (float)sh;
        if (boxH > sh) boxH = (float)sh;
        float boxW = boxH * 0.45f;
        g_espEntries.push_back({sx, sy, boxW, boxH, hpRatio, dist, "Person"});

        float dx = sx - sw / 2.0f;
        float dy = sy - sh / 2.0f;
        float score = dx * dx + dy * dy;
        if (score < bestScore) {
            bestScore = score;
            Vector3 camPos;
            Il2CppObject* camTransform = nullptr;
            if (get_transform(camera, camTransform) && get_position(camTransform, camPos)) {
                best.valid = true;
                best.headPos = headPos;
                best.camPos = camPos;
                best.timestampMs = now_ms();
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(g.targetMutex);
        g.target = best;
    }
    if (g.zoomGetIsZoomingIn) {
        bool z = false;
        invoke_value<bool>(g.zoomGetIsZoomingIn, nullptr, nullptr, z);
        g_isZooming = z;
    }
}

void write_esp() {
    std::string json = "{\"mode\":\"";
    json += (g.mode == Mode::MISI ? "MISI" : g.mode == Mode::ARENA ? "ARENA" : "UNKNOWN");
    json += "\",\"t\":[";
    if (g.config.esp && g.mode == Mode::MISI) {
        bool first = true;
        for (const auto& e : g_espEntries) {
            if (!first) json += ",";
            first = false;
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "{\"x\":%.1f,\"y\":%.1f,\"w\":%.1f,\"h\":%.1f,\"hp\":%.2f,\"d\":%.1f,\"n\":\"%s\"}",
                          e.x, e.y, e.w, e.h, e.hp, e.dist, e.name.c_str());
            json += buf;
        }
    }
    json += "]}";
    std::string tmp = std::string(kEspPath) + ".tmp";
    if (write_file(tmp.c_str(), json)) {
        rename(tmp.c_str(), kEspPath);
    }
}

// ---------------------------------------------------------------- features
WeaponDataOrig& orig_for(void* dataObj) {
    return g.weaponDataOrig[dataObj];
}

void apply_features(const Config& prev) {
    if (g.mode != Mode::MISI) return;
    auto weapons = find_objects(g.weaponClass);

    // No-spread: _spreadAmount = 0 + WeaponData.Spread = 0.
    // v1.0.2: _spreadAmount selalu di-nol-kan di misi meski toggle OFF.
    for (Il2CppObject* weapon : weapons) {
        if (!g.object_get_class(weapon)) continue;  // objek sudah di-destroy
        float zero = 0.0f;
        if (g.config.noSpread) {
            field_set(weapon, g.weaponSpreadAmount, zero);
        }
        if (g.config.noSpread) {
            Il2CppObject* data = nullptr;
            field_get(weapon, g.weaponDataField, data);
            if (data && g.dataSetSpread && g.dataGetSpread) {
                WeaponDataOrig& o = orig_for(data);
                if (!o.saved) {
                    float s = 0;
                    invoke_value<float>(g.dataGetSpread, data, nullptr, s);
                    o.spread = s;
                    o.saved = true;
                }
                float z = 0;
                void* params[1] = {&z};
                invoke_obj(g.dataSetSpread, data, params);
            }
        }
    }
    // No-spread dimatikan → kembalikan Spread senjata yang masih hidup.
    if (prev.noSpread && !g.config.noSpread && g.dataSetSpread) {
        for (Il2CppObject* weapon : weapons) {
            if (!g.object_get_class(weapon)) continue;  // objek sudah di-destroy
            Il2CppObject* data = nullptr;
            field_get(weapon, g.weaponDataField, data);
            if (!data) continue;
            auto it = g.weaponDataOrig.find(data);
            if (it != g.weaponDataOrig.end() && it->second.saved) {
                float s = it->second.spread;
                void* params[1] = {&s};
                invoke_obj(g.dataSetSpread, data, params);
            }
        }
    }

    // Shooter (pemain) — no reload + zoom.
    auto shooters = find_objects(g.shooterClass);
    if (!shooters.empty()) {
        Il2CppObject* shooter = shooters[0];
        if (!g.object_get_class(shooter)) return;  // objek sudah di-destroy
        Il2CppObject* data = nullptr;
        field_get(shooter, g.shooterWeaponData, data);
        if (data) {
            WeaponDataOrig& o = orig_for(data);
            if (!o.saved) {
                float s = 0, r = 0, m = 0;
                if (g.dataGetSpread) invoke_value<float>(g.dataGetSpread, data, nullptr, s);
                if (g.dataGetReloadTime) invoke_value<float>(g.dataGetReloadTime, data, nullptr, r);
                if (g.dataGetMaxZoom) invoke_value<float>(g.dataGetMaxZoom, data, nullptr, m);
                o.spread = s;
                o.reloadTime = r;
                o.maxZoom = m;
                o.saved = true;
            }
            if (g.config.noReload && g.dataSetReloadTime && g.shooterSetCurrentAmmo &&
                g.dataGetClipSize) {
                // ReloadTime = 0 (reload instan) + ammo selalu penuh.
                float z = 0;
                void* params[1] = {&z};
                invoke_obj(g.dataSetReloadTime, data, params);
                int clip = 0;
                if (invoke_value<int>(g.dataGetClipSize, data, nullptr, clip) && clip > 0) {
                    void* ap[1] = {&clip};
                    invoke_obj(g.shooterSetCurrentAmmo, shooter, ap);
                }
            }
            if (prev.noReload && !g.config.noReload && g.dataSetReloadTime && o.saved) {
                float r = o.reloadTime;
                void* params[1] = {&r};
                invoke_obj(g.dataSetReloadTime, data, params);
            }
            if (g.config.zoomFar && g.dataSetMaxZoom) {
                float mz = 30.0f;
                void* params[1] = {&mz};
                invoke_obj(g.dataSetMaxZoom, data, params);
            }
            if (prev.zoomFar && !g.config.zoomFar && g.dataSetMaxZoom && o.saved) {
                float m = o.maxZoom;
                void* params[1] = {&m};
                invoke_obj(g.dataSetMaxZoom, data, params);
            }
        }
    }

    // Zoom jauh — InputUtils.Zoom.ScaleMax (static field).
    // Guard pointer API-nya: load_il2cpp_api mencatat bila simbol tak
    // ada (g.failed) tapi tidak menggagalkan resolve — pemanggilan
    // function pointer null di sini akan SIGSEGV.
    if (g.config.zoomFar && g.zoomScaleMax && g.field_static_get_value &&
        g.field_static_set_value) {
        if (!g.zoomScaleMaxSaved) {
            float orig = 0;
            g.field_static_get_value(g.zoomScaleMax, &orig);
            g.zoomScaleMaxOrig = orig;
            g.zoomScaleMaxSaved = true;
        }
        float v = 30.0f;
        g.field_static_set_value(g.zoomScaleMax, &v);
    }
    if (prev.zoomFar && !g.config.zoomFar && g.zoomScaleMax && g.zoomScaleMaxSaved &&
        g.field_static_set_value) {
        float v = g.zoomScaleMaxOrig;
        g.field_static_set_value(g.zoomScaleMax, &v);
    }
}

// ---------------------------------------------------------------- aimbot hook
typedef Il2CppArray* (*ShootTestFn)(void* thiz, Ray* ray, int32_t layerMask, void* method);
std::map<void*, ShootTestFn> g_origShootTest;

Il2CppArray* hook_ShootTest(void* thiz, Ray* ray, int32_t layerMask, void* method) {
    (void)thiz;
    void* mp = method ? reinterpret_cast<Il2CppMethodOpaque*>(method)->methodPointer : nullptr;
    auto it = mp ? g_origShootTest.find(mp) : g_origShootTest.end();
    if (it == g_origShootTest.end()) {
        // Seharusnya tak terjadi (hook hanya dipasang bila orig tercatat).
        // Kembalikan null supaya tembakan gagal aman, dan catat ke log.
        LOGE("hook_ShootTest: orig tak dikenal untuk method %p", mp);
        return nullptr;
    }
    ShootTestFn orig = it->second;
    bool wantAim = g.config.aimbot || (g.config.zoomAim && g_isZooming);
    if (wantAim && g.mode == Mode::MISI && ray) {
        CachedTarget t;
        {
            std::lock_guard<std::mutex> lock(g.targetMutex);
            t = g.target;
        }
        if (t.valid && (now_ms() - t.timestampMs) < 300) {
            Vector3 dir{t.headPos.x - t.camPos.x, t.headPos.y - t.camPos.y,
                        t.headPos.z - t.camPos.z};
            float len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
            if (len > 0.001f) {
                ray->origin = t.camPos;
                ray->direction = {dir.x / len, dir.y / len, dir.z / len};
            }
        }
    }
    return orig(thiz, ray, layerMask, method);
}

void try_install_hook() {
    auto shooters = find_objects(g.shooterClass);
    if (shooters.empty()) return;
    if (!g.object_get_class(shooters[0])) return;  // objek sudah di-destroy
    Il2CppObject* weapon = nullptr;
    field_get(shooters[0], g.shooterWeapon, weapon);
    if (!weapon) return;
    Il2CppClass* wclass = g.object_get_class(weapon);
    if (!wclass) return;
    Il2CppMethod* m = find_method(wclass, "ShootTest", 2);
    if (!m) {
        if (!g.hookInstalled) {
            if (std::find(g.failed.begin(), g.failed.end(), "method:ShootTest") == g.failed.end()) {
                g.failed.push_back("method:ShootTest");
            }
        }
        return;
    }
    void* addr = reinterpret_cast<Il2CppMethodOpaque*>(m)->methodPointer;
    if (!addr) return;
    if (g_origShootTest.count(addr)) return;  // sudah ter-hook
    ShootTestFn orig = nullptr;
    int rc = DobbyHook(addr, reinterpret_cast<void*>(&hook_ShootTest),
                       reinterpret_cast<void**>(&orig));
    if (rc == 0 && orig) {
        g_origShootTest[addr] = orig;
        g.hookInstalled = true;
        LOGI("ShootTest hooked at %p", addr);
    } else {
        LOGE("DobbyHook ShootTest failed rc=%d", rc);
    }
}

// ---------------------------------------------------------------- worker
void* worker_main(void*) {
    // Jejak pertama SEBELUM resolve: bila crash terjadi saat resolve,
    // status INIT tetap ada (v1.0.2: tanpa ini, crash sebelum READY
    // tidak meninggalkan jejak apa pun).
    write_status("INIT");
    // Resolve (retry s.d. 60 dtk — libil2cpp mungkin belum siap).
    long long start = now_ms();
    while (!g.coreResolved) {
        if (resolve_all()) break;
        if (now_ms() - start > 60000) {
            write_status("UNSUPPORTED");
            LOGE("resolve gagal setelah 60 dtk — UNSUPPORTED");
            return nullptr;
        }
        usleep(500 * 1000);
    }
    write_status(g.failed.empty() ? "READY" : "PARTIAL");
    LOGI("resolve selesai: %s (failed=%zu)",
         g.failed.empty() ? "READY" : "PARTIAL", g.failed.size());
    read_config();

    Config prev = g.config;
    while (true) {
        long long t = now_ms();
        if (t - g.lastConfigReadMs >= 500) {
            read_config();
            g.lastConfigReadMs = t;
        }
        if (t - g.lastModeCheckMs >= 500) {
            update_mode();
            g.lastModeCheckMs = t;
        }
        if (t - g.lastHookCheckMs >= 2000) {
            try_install_hook();
            g.lastHookCheckMs = t;
        }
        if (g.mode == Mode::MISI) {
            enumerate_and_target();
            apply_features(prev);
        } else {
            std::lock_guard<std::mutex> lock(g.targetMutex);
            g.target.valid = false;
        }
        prev = g.config;
        if (t - g.lastEspWriteMs >= 150) {
            write_esp();
            g.lastEspWriteMs = t;
        }
        if (t - g.lastStatusWriteMs >= 2000) {
            write_status(g.failed.empty() ? "READY" : "PARTIAL");
            g.lastStatusWriteMs = t;
        }
        usleep(100 * 1000);
    }
    return nullptr;
}

}  // namespace (anonymous)

// start_worker sengaja DI LUAR anonymous namespace: ia dideklarasikan di
// sniper3d.h dan dipanggil dari payload_main.cpp — bila ikut masuk
// anonymous namespace, linkage-nya internal dan linker tak menemukannya.
void start_worker() {
    // v1.0.2 hanya menangkap SIGSEGV/SIGABRT/SIGBUS — crash SIGILL /
    // SIGFPE / SIGTRAP / SIGSYS tidak meninggalkan STATUS=CRASH sama
    // sekali ("gak ada" di tes POCO v1.0.2).
    signal(SIGSEGV, crash_handler);
    signal(SIGABRT, crash_handler);
    signal(SIGBUS, crash_handler);
    signal(SIGILL, crash_handler);
    signal(SIGFPE, crash_handler);
    signal(SIGTRAP, crash_handler);
    signal(SIGSYS, crash_handler);
    pthread_t th;
    if (pthread_create(&th, nullptr, worker_main, nullptr) == 0) {
        pthread_detach(th);
    }
}

}  // namespace sniper3d
