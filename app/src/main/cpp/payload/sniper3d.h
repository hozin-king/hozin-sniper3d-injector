// Hozin Sniper 3D Injector — logika payload (di dalam proses game).
//
// Fitur (semua default OFF, semua WAJIB gate misi — fail-closed):
//   aimbot/aimlock, auto-aim saat zoom, ESP, no-spread (= no recoil),
//   no reload, zoom jauh.
// Gate: aktif HANYA bila LevelController (misi) jalan DAN
// ArenaIngameController / ArenaMatchFluxController TIDAK jalan. Bila
// mode tak bisa dipastikan → semua fitur MATI. TIDAK ADA toggle Arena.
#pragma once

namespace sniper3d {

// Dipanggil sekali dari constructor payload (harus kembali CEPAT —
// semua kerja berat jalan di worker thread sendiri).
void start_worker();

}  // namespace sniper3d
