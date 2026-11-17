package com.hozinking.sniper3dinjector

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.os.Bundle
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import com.hozinking.sniper3dinjector.databinding.ActivityMainBinding

/**
 * Hozin Sniper 3D Injector v1.0.1 — launcher root untuk Sniper 3D
 * (com.fungames.sniper3d) v7.15.0, Unity 6 IL2CPP. Game TIDAK dibundel —
 * user install sendiri dari Play Store.
 *
 * ALUR (otomatis penuh): Tap "Inject ke Sniper 3D" → cek root → cek
 * install/versi → bila game belum jalan, OTOMATIS launch game → tunggu
 * PID → tunggu libil2cpp.so termuat → jeda settle 3 dtk (jangan dlopen
 * saat main thread masih memegang loader lock — pelajaran TR v2.0/v2.1)
 * → inject otomatis (retry 3x) → tunggu status payload 15 dtk.
 * Tombol mengambang (FloatingService) muncul begitu proses dimulai dan
 * tetap tampil apa pun hasilnya — anti gagal.
 *
 * Gate versi ada di payload (resolve class/method IL2CPP berdasarkan
 * nama — bukan versionCode). Fitur HANYA aktif di mode misi (solo vs
 * AI); di Arena semua MATI otomatis (fail-closed).
 *
 * Diagnosis di dalam aplikasi (aturan 14 Nov — JANGAN suruh user
 * jalankan Termux/logcat): log injector langkah-demi-langkah tampil di
 * layar + tombol "Salin diagnosis".
 */
class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding

    @Volatile
    private var autoRunning = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        // Izin overlay diminta sekali di awal supaya tombol mengambang
        // bisa tampil; bila ditolak, alur tetap jalan tanpa overlay.
        if (!FloatingService.canDrawOverlays(this)) {
            FloatingService.requestPermission(this)
            log("Izin 'Tampilkan di atas aplikasi lain' diminta — izinkan supaya menu mengambang bisa muncul.")
        }

        binding.btnCheckRoot.setOnClickListener {
            Thread {
                val available = RootShell.isRootAvailable()
                log(if (available) "root: available" else "root: NOT available — butuh HP root")
            }.start()
        }

        binding.btnCheckGame.setOnClickListener {
            Thread {
                log(InjectorBridge.checkGame(this))
            }.start()
        }

        binding.btnInject.setOnClickListener {
            if (autoRunning) {
                log("proses inject masih jalan — tunggu selesai")
                return@setOnClickListener
            }
            Thread { runAutoInject() }.start()
        }

        binding.btnCopyDiag.setOnClickListener {
            val text = binding.textLog.text.toString()
            val cm = getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
            cm.setPrimaryClip(ClipData.newPlainText("HozinSniper3D diagnosis", text))
            Toast.makeText(this, "Diagnosis disalin", Toast.LENGTH_SHORT).show()
        }
    }

    /** Seluruh alur inject otomatis, dengan progres ke log. */
    private fun runAutoInject() {
        autoRunning = true
        try {
            log("== Inject v1.0.1 — alur otomatis ==")
            // Tombol mengambang muncul duluan — anti gagal.
            FloatingService.start(this)

            // 1. Root.
            if (!RootShell.isRootAvailable()) {
                log("root: NOT available — butuh HP root")
                return
            }
            log("root: available")

            // 2. Install / versi (cross-check PID — jangan pernah bilang
            // "TIDAK terinstall" saat game jalan).
            val check = InjectorBridge.checkGame(this)
            log(check)
            val version = InjectorBridge.installedVersionCode(this)
            val versionName = InjectorBridge.installedVersionName(this)
            if (version != null) {
                log("versi terinstall: ${versionName ?: "?"} (code $version) — ditampilkan apa adanya")
            }
            if (version == null && InjectorBridge.findPid(InjectorBridge.TARGET_PACKAGE) == null) {
                log("Sniper 3D belum terinstall")
                return
            }

            // 3. Game belum jalan → luncurkan otomatis.
            var pid = InjectorBridge.findPid(InjectorBridge.TARGET_PACKAGE)
            if (pid == null) {
                log("Sniper 3D belum jalan — meluncurkan otomatis…")
                if (!InjectorBridge.launchGame(this)) {
                    log("launch game GAGAL — buka Sniper 3D manual lalu tap Inject lagi")
                    return
                }
                pid = waitForPid(30)
                if (pid == null) {
                    log("game tidak muncul (PID tak ketemu setelah 30 dtk)")
                    return
                }
            }
            log("pid Sniper 3D: $pid")

            // 4. Tunggu libil2cpp.so termuat (maks 45 dtk).
            val libOk = waitForLib(pid, 45)
            if (libOk) {
                log("libil2cpp.so termuat")
                log("jeda settle 3 dtk biar game selesai load…")
                Thread.sleep(3000)
                val pidAlive = InjectorBridge.findPid(InjectorBridge.TARGET_PACKAGE)
                if (pidAlive != pid) {
                    log("PID game berubah/mati saat jeda settle — batal")
                    return
                }
            } else {
                log("libil2cpp.so belum terlihat setelah 45 dtk — inject tetap dicoba (payload menunggu sendiri)")
            }

            // 5. Inject + diagnosis.
            val result = InjectorBridge.inject(this, pid)
            if (result.diag.isNotEmpty()) log(result.diag)
            log("injector exit=${result.exitCode}")
            val pidAfter = InjectorBridge.findPid(InjectorBridge.TARGET_PACKAGE)
            log(
                if (pidAfter != null) "game masih jalan setelah inject (pid $pidAfter)"
                else "game MATI setelah inject — kemungkinan crash saat remote call"
            )
            if (result.exitCode == 0) {
                val ps = waitForPayloadStatus(15)
                val rawStatus = InjectorBridge.readPayloadStatus()
                log("--- isi file status payload ---")
                log(if (rawStatus.isNotEmpty()) rawStatus else "(file status payload belum ada)")
                val ctorLog = InjectorBridge.readCtorLog()
                if (ctorLog.isNotEmpty()) log("--- ctor log ---\n$ctorLog")
                val pidFinal = InjectorBridge.findPid(InjectorBridge.TARGET_PACKAGE)
                log(
                    if (pidFinal != null) "game: masih jalan (pid $pidFinal)"
                    else "game: MATI — crash saat init payload (cari STATUS=CRASH di atas)"
                )
                when {
                    ps.status == "READY" -> {
                        log("payload READY — semua class/method IL2CPP ketemu (lib ${ps.libSize} B, hook ${ps.hook})")
                        log("inject OK — kembali ke game, tap tombol 🎯 untuk menu mod (mode MISI saja)")
                    }
                    ps.status.startsWith("PARTIAL") -> {
                        log("payload PARTIAL (${ps.status}) — sebagian resolve gagal: ${ps.failed} — " +
                            "game tetap jalan, fitur yang gagal nonaktif (bukan toggle palsu)")
                    }
                    ps.status.startsWith("CRASH") -> {
                        log("payload CRASH saat init: ${ps.status} — langkah terakhir ada di STEP di atas. Salin diagnosis ini.")
                    }
                    ps.status == "UNSUPPORTED" -> {
                        val v = versionName ?: "?"
                        val c = version ?: -1
                        log("versi $v (code $c) BELUM didukung — tidak ketemu: ${ps.failed}")
                        log("BuildID libil2cpp.so tidak tercatat — payload memakai nama, bukan offset")
                    }
                    else -> {
                        log("inject exit 0 (dlopen OK) tapi status payload tak terbaca setelah 15 dtk — " +
                            "payload mungkin masih init, atau file status tak bisa ditulis. Tap Inject ulang.")
                    }
                }
            } else {
                log("inject GAGAL — ${InjectorBridge.failureReason(result.exitCode)}")
            }
            // Bawa user ke game (pola Traffic Rider) — jangan biarkan user
            // menatap layar log sementara game jalan di belakang.
            // Hanya bila game masih hidup; bila game mati, tetap di sini
            // supaya diagnosis bisa disalin.
            if (pidAfter != null) {
                log("kembali ke game…")
                InjectorBridge.bringGameToFront()
                runOnUiThread { moveTaskToBack(true) }
            }
        } finally {
            autoRunning = false
        }
    }

    /** Poll PID sampai timeout detik; null bila tak muncul. */
    private fun waitForPid(timeoutSec: Int): Int? {
        repeat(timeoutSec * 2) {
            val pid = InjectorBridge.findPid(InjectorBridge.TARGET_PACKAGE)
            if (pid != null) return pid
            Thread.sleep(500)
        }
        return null
    }

    /** Poll file status payload sampai timeout detik; status "" bila tak muncul. */
    private fun waitForPayloadStatus(timeoutSec: Int): InjectorBridge.PayloadStatus {
        repeat(timeoutSec * 2) {
            val raw = InjectorBridge.readPayloadStatus()
            val ps = InjectorBridge.parsePayloadStatus(raw)
            if (ps.status.isNotEmpty()) return ps
            Thread.sleep(500)
        }
        return InjectorBridge.PayloadStatus("", "-", "-", "-", "-", "")
    }

    /** Poll libil2cpp.so di /proc/<pid>/maps sampai timeout detik. */
    private fun waitForLib(pid: Int, timeoutSec: Int): Boolean {
        var waited = 0
        while (waited < timeoutSec) {
            if (InjectorBridge.isLibLoaded(pid, InjectorBridge.TARGET_LIB)) return true
            Thread.sleep(1000)
            waited++
        }
        return InjectorBridge.isLibLoaded(pid, InjectorBridge.TARGET_LIB)
    }

    private fun log(line: String) {
        runOnUiThread {
            binding.textLog.append(line + "\n")
        }
    }
}
