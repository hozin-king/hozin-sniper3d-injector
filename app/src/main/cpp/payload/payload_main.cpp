// Hozin Sniper 3D Injector — payload entry point.
//
// Constructor WAJIB kembali CEPAT: ia hanya menulis ctor log, lalu
// spawn worker thread (sniper3d::start_worker) dan kembali. Semua
// kerja berat (resolve IL2CPP, hook, fitur) jalan di worker thread —
// kalau constructor menahan dlopen, remote call injector (budget
// dlopen 10 dtk sejak v1.0.3) timeout dan MainActivity mengira inject gagal.
//
// Ctor log membuktikan constructor sempat KEMBALI:
//   CTOR=masuk  → constructor mulai
//   CTOR=kembali → constructor selesai (worker jalan)
// Bila file hanya berisi "masuk" (tanpa "kembali"), constructor macet
// di tengah — diagnosis langsung, tanpa logcat.
#include "sniper3d.h"

#include <android/log.h>
#include <cstdio>
#include <string>

#define LOG_TAG "HozinSniper3D"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

namespace {

constexpr const char* kCtorCache = "/data/data/com.fungames.sniper3d/cache/hozinsniper_ctor.log";
constexpr const char* kCtorTmp = "/data/local/tmp/hozinsniper_ctor.log";

void write_ctor(const char* line) {
    std::string content = line;
    content += "\n";
    FILE* f = std::fopen(kCtorCache, "a");
    if (f) {
        std::fwrite(content.data(), 1, content.size(), f);
        std::fclose(f);
    }
    f = std::fopen(kCtorTmp, "a");
    if (f) {
        std::fwrite(content.data(), 1, content.size(), f);
        std::fclose(f);
    }
}

}  // namespace

__attribute__((constructor)) static void on_load() {
    write_ctor("CTOR=masuk");
    LOGI("payload Hozin Sniper 3D v1.0.5 masuk ke proses game");
    sniper3d::start_worker();
    write_ctor("CTOR=kembali");
    LOGI("payload constructor kembali — worker jalan");
}
