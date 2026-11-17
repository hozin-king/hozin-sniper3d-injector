# Hozin Sniper 3D Injector v1.0.1

Root injector (Kotlin + payload native) untuk **Sniper 3D** (`com.fungames.sniper3d`)
**v7.15.0 / code 33186**, Unity 6000.3.21f1 (Unity 6) IL2CPP. Game **tidak dibundel**
— user install sendiri dari Play Store.

## Fitur (semua default OFF)

1. **Aimbot / aimlock** — hook `Player.Weapon.ShootTest(Ray, LayerMask)` pada class
   konkret senjata pemain; ray ditulis ulang ke kepala musuh terdekat dari tengah
   layar (enumerasi `Person` → `_hitTransform`).
2. **Auto-aim saat zoom** — aimbot yang sama, aktif hanya saat
   `InputUtils.Zoom.IsZoomingIn`.
3. **ESP** — overlay Canvas di proses aplikasi (kotak hijau, bar HP
   `_currentHealth`/`BaseHealth`, nama + jarak, crosshair). Data posisi dihitung
   payload lewat `Camera.main.WorldToScreenPoint`, dikirim lewat file JSON di
   cache game. Primitif gambar mengikuti referensi `ESP_3_thnu.h`.
4. **No-spread (= no recoil)** — game ini tidak punya field "recoil";
   yang di-nol-kan adalah `Weapon._spreadAmount` + `WeaponData.Spread`.
5. **No reload** — `CharacterShooter.CurrentAmmo` selalu penuh +
   `WeaponData.ReloadTime = 0`.
6. **Zoom jauh** — `InputUtils.Zoom.ScaleMax` + `WeaponData.MaxZoom` = 30.

## Gate mode misi (fail-closed, terpusat di payload)

Fitur aktif **hanya** bila `LevelController` (misi) jalan **dan**
`ArenaIngameController` / `ArenaMatchFluxController` **tidak** jalan.
Bila mode tak bisa dipastikan → semua fitur **MATI**. Tidak ada toggle Arena
di menu. Dasarnya: hasil REA (`~/workspace/rea-sniper3d/REPORT.md`) — misi
solo lawan AI terverifikasi lokal; Arena adalah PvP real-time dengan validasi
tembakan server + tag cheater (ban permanen). Klaim "Arena cuma bot" **tidak
terbukti** — bukan alasan menambah fitur Arena.

## Yang sengaja TIDAK dibangun

Fitur Arena/PvP apa pun, koin/diamond/energi, IAP/unlock uang asli, hadiah
misi, damage multiplier, bypass Play Integrity, pemalsuan laporan progres
(`SyncPlayerCampaign`), speed/teleport/noclip.

## Teknis

- **Tanpa offset hardcoded.** Semua class/method/field IL2CPP di-resolve runtime
  berdasarkan nama. Bila game update dan sebuah nama hilang, payload melaporkan
  `PARTIAL`/`UNSUPPORTED` + nama yang gagal ke file status — fitur yang gagal
  nonaktif, bukan toggle palsu.
- **Dobby** (hook) commit `5dfc854` + 5 patch NDK modern (di `CMakeLists.txt`,
  pola yang sama dipakai `unity-controller-root`).
- **Injector**: ptrace remote dlopen (arm64), payload di-stage ke cache game
  sendiri (bukan `/data/local/tmp` — SELinux sering menolak dlopen dari sana),
  retry 3x, tangkap `dlerror()` remote.
- **Diagnosis in-app**: file status payload (`STATUS`/`FAILED`/`LIBSIZE`/`MODE`/
  `HOOK`) + ctor log + tombol salin diagnosis. Jangan suruh user jalankan
  Termux/logcat.
- **Standar 16 Nov**: game auto-launch bila belum jalan; tombol mengambang
  `🎯` + menu selalu tampil apa pun hasil inject.
- **Keystore persisten**: `debug.keystore` di repo TIDAK di-push (`.gitignore`);
  CI memulihkannya dari secret `DEBUG_KEYSTORE_BASE64`. `signingConfigs.debug`
  kondisional — fallback ke keystore ephemeral bila file tak ada.

## Risiko (jujur)

- **Tanpa jaminan anti-ban.** Server punya Play Integrity + tag cheater
  (`CheaterDetected`, `PlayerIsTaggedAsCheater`) → ban/suspend permanen.
  Pakai akun utama dengan risiko sendiri.
- **v1.0.1 memperbaiki kegagalan total v1.0 di perangkat** (inject exit=127):
  manifest v1.0 tidak menyetel `extractNativeLibs` → `.so` tidak diekstrak
  ke lib dir → shell melaporkan "command not found". v1.0.1 menyetel
  `extractNativeLibs=true` (pola HCR) + jaring pengaman ekstraksi dari APK
  + auto-launch game ke foreground via `am start` (pola Traffic Rider).
  Hook `ShootTest` tetap bagian paling berisiko (ABI Ray 24-byte + virtual
  override). Bila hook gagal, status payload menunjukkan `HOOK=gagal` dan
  aimbot nonaktif — fitur lain tetap bisa jalan.

## Build

APK **hanya** di GitHub Actions: push ke `main` → build → tag + rilis otomatis
(`HozinSniper3DInjector-v1.0.1.apk`).

## Struktur

```
app/src/main/java/com/hozinking/sniper3dinjector/
  MainActivity.kt      # alur inject otomatis + diagnosis
  RootShell.kt         # shell su persisten
  InjectorBridge.kt    # staging, inject, status, config, ESP (via root)
  FloatingService.kt   # tombol 🎯 + menu 6 toggle + polling ESP
  EspView.kt           # kanvas ESP (kotak, bar HP, teks, crosshair)
app/src/main/cpp/
  CMakeLists.txt       # Dobby FetchContent + 5 patch
  injector/            # ptrace remote dlopen (libinjector.so)
  payload/             # payload IL2CPP (libpayload.so)
    il2cpp_api.h       # deklarasi minimal IL2CPP
    sniper3d.h/.cpp    # resolve, gate, aimbot hook, fitur, ESP
    payload_main.cpp   # constructor cepat + ctor log
```
