package com.hozinking.sniper3dinjector

import android.content.Context
import android.content.pm.PackageManager
import java.io.File
import java.util.zip.ZipFile

/**
 * Jembatan Kotlin ↔ native injector (ptrace + remote dlopen) untuk
 * Sniper 3D v7.15.0 (com.fungames.sniper3d).
 *
 * Pola staging (pelajaran PvZ v1.1/v1.2 + HCR v1.10): payload HARUS di
 * cache game sendiri (/data/data/<game>/cache, konteks app_data_file
 * milik game) karena /data/local/tmp sering tak bisa dibaca proses game
 * (SELinux) — dlopen dari sana NULL. Injector menangkap dlerror() dan
 * mencoba jalur cadangan.
 *
 * v1.0.2: SELURUH alur root + launch ditulis ulang mengikuti kerangka
 * Traffic Rider (ModInjector.java + RootUtil.java, teruji di POCO user):
 * tiap perintah root lewat `su -c` baru dengan timeout keras (RootUtil),
 * game diluncurkan DULUAN sebelum semua tunggu yang lama, dan dibawa
 * ke depan HANYA bila inject sukses. RootShell persisten v1.0–v1.0.1
 * DIHAPUS (tanpa timeout — alur bisa membeku selamanya, bukti tes
 * v1.0.1: log berhenti di "root: available").
 *
 * Gate versi: aplikasi TIDAK menolak berdasarkan versionCode. Gate ada
 * di payload — semua class/method IL2CPP harus ter-resolve berdasarkan
 * nama. Setelah inject, aplikasi membaca file status payload (±15 dtk)
 * dan menampilkan READY / PARTIAL / UNSUPPORTED / CRASH + nama yang gagal.
 *
 * Config toggle (ditulis aplikasi, dibaca payload tiap 500 ms) dan data
 * ESP (ditulis payload, dibaca overlay aplikasi) sama-sama tinggal di
 * cache game dan diakses aplikasi lewat root.
 */
object InjectorBridge {

    const val TARGET_PACKAGE = "com.fungames.sniper3d"
    const val TARGET_LIB = "libil2cpp.so"

    private const val INJECTOR_LIB = "libinjector.so"
    private const val PAYLOAD_LIB = "libpayload.so"

    private const val PAYLOAD_TMP = "/data/local/tmp/hozinsniper_payload.so"
    private const val PAYLOAD_GAME =
        "/data/data/com.fungames.sniper3d/cache/hozinsniper_payload.so"
    private const val GAME_CACHE_DIR = "/data/data/com.fungames.sniper3d/cache"
    private const val GAME_DATA_DIR = "/data/data/com.fungames.sniper3d"
    const val INJECT_LOG = "/data/local/tmp/hozinsniper_inject.log"

    /** File status payload (payload menulis ke yang pertama bisa). */
    private const val PAYLOAD_STATUS_TMP = "/data/local/tmp/hozinsniper_payload.log"
    private const val PAYLOAD_STATUS_CACHE =
        "/data/data/com.fungames.sniper3d/cache/hozinsniper_payload.log"

    /** Marker constructor payload — apakah constructor sempat kembali. */
    private const val CTOR_LOG_CACHE =
        "/data/data/com.fungames.sniper3d/cache/hozinsniper_ctor.log"

    /** Config toggle + data ESP (keduanya di cache game). */
    const val CONFIG_PATH = "$GAME_CACHE_DIR/hozinsniper_config.json"
    const val ESP_PATH = "$GAME_CACHE_DIR/hozinsniper_esp.json"

    data class InjectResult(val exitCode: Int, val output: String, val diag: String)

    /** Status payload, diparse dari hozinsniper_payload.log. */
    data class PayloadStatus(
        val status: String,      // READY | PARTIAL | UNSUPPORTED | CRASH | ""
        val failed: String,      // daftar nama yang gagal resolve, "-" bila tak ada
        val libSize: String,
        val mode: String,        // MISI | ARENA | UNKNOWN | "-"
        val hook: String,        // ok | gagal | "-"
        val raw: String
    )

    fun injectorPath(context: Context): String =
        File(context.applicationInfo.nativeLibraryDir, INJECTOR_LIB).absolutePath

    fun payloadPath(context: Context): String =
        File(context.applicationInfo.nativeLibraryDir, PAYLOAD_LIB).absolutePath

    /**
     * Cari file .so native: utama nativeLibraryDir; bila tidak ada di
     * sana, ekstrak dari APK sendiri (sourceDir) ke filesDir + chmod 755.
     * Return path absolut, atau null bila keduanya gagal.
     *
     * Latar: v1.0 gagal total dengan exit=127 karena manifest TIDAK
     * menyetel extractNativeLibs (AGP default false) → .so hanya
     * tersimpan di APK dan TIDAK PERNAH diekstrak ke nativeLibraryDir,
     * jadi shell su melaporkan "command not found". v1.0.1 menyetel
     * extractNativeLibs=true di manifest (akar diperbaiki, pola HCR);
     * fungsi ini adalah jaring pengaman bila file tetap tidak ada.
     */
    fun resolveNativeLib(context: Context, libName: String): String? {
        val inLibDir = File(context.applicationInfo.nativeLibraryDir, libName)
        if (inLibDir.exists()) return inLibDir.absolutePath
        val dest = File(context.filesDir, libName)
        if (dest.exists() && dest.length() > 0) return dest.absolutePath
        return try {
            val apk = ZipFile(context.applicationInfo.sourceDir)
            val entry = apk.getEntry("lib/arm64-v8a/$libName") ?: return null
            apk.getInputStream(entry).use { inp ->
                dest.outputStream().use { out -> inp.copyTo(out) }
            }
            RootUtil.su("chmod 755 '${dest.absolutePath}'")
            if (dest.exists() && dest.length() > 0) dest.absolutePath else null
        } catch (e: Exception) {
            null
        }
    }

    /** Cek game terinstall + tampilkan versi apa adanya (tanpa penilaian). */
    fun checkGame(context: Context): String {
        val pid = findPid(TARGET_PACKAGE)
        val pidNote = if (pid != null) " (jalan, pid $pid)" else " (belum jalan)"
        val info = try {
            context.packageManager.getPackageInfo(TARGET_PACKAGE, 0)
        } catch (e: PackageManager.NameNotFoundException) {
            null
        }
        if (info != null) {
            // longVersionCode baru ada di API 28; di bawah itu pakai versionCode.
            val code = if (android.os.Build.VERSION.SDK_INT >= 28) info.longVersionCode
            else info.versionCode.toLong()
            val name = info.versionName ?: "?"
            return "Sniper 3D terinstall: $name ($code)$pidNote — target v7.15.0 (code 33186); " +
                "bila versi berbeda, payload yang memutuskan (resolve berdasarkan nama)"
        }
        if (pid != null) {
            return "Sniper 3D JALAN (pid $pid) tapi info versi tak terbaca via PackageManager — " +
                "inject tetap dicoba, payload yang memverifikasi (fail-closed)"
        }
        return "Sniper 3D TIDAK terinstall — install com.fungames.sniper3d dari Play Store dulu"
    }

    fun installedVersionCode(context: Context): Long? {
        return try {
            val info = context.packageManager.getPackageInfo(TARGET_PACKAGE, 0)
            if (android.os.Build.VERSION.SDK_INT >= 28) info.longVersionCode
            else info.versionCode.toLong()
        } catch (e: PackageManager.NameNotFoundException) {
            null
        }
    }

    fun installedVersionName(context: Context): String? {
        return try {
            context.packageManager.getPackageInfo(TARGET_PACKAGE, 0).versionName
        } catch (e: PackageManager.NameNotFoundException) {
            null
        }
    }

    /** Baca file status payload (dua lokasi, via root). "" bila tak ada. */
    fun readPayloadStatus(): String {
        val r = RootUtil.su(
            "cat '$PAYLOAD_STATUS_TMP' 2>/dev/null || cat '$PAYLOAD_STATUS_CACHE' 2>/dev/null"
        )
        return r.output.trim()
    }

    /** Baca ctor log payload. "" bila tak ada. */
    fun readCtorLog(): String {
        val r = RootUtil.su("cat '$CTOR_LOG_CACHE' 2>/dev/null")
        return r.output.trim()
    }

    /** Parse isi file status payload. status "" = file belum ada/tak terbaca. */
    fun parsePayloadStatus(raw: String): PayloadStatus {
        if (raw.isEmpty()) return PayloadStatus("", "-", "-", "-", "-", "")
        var status = ""
        var failed = "-"
        var libSize = "-"
        var mode = "-"
        var hook = "-"
        for (line in raw.lines()) {
            val parts = line.split("=", limit = 2)
            if (parts.size != 2) continue
            when (parts[0]) {
                "STATUS" -> status = parts[1]
                "FAILED" -> failed = parts[1]
                "LIBSIZE" -> libSize = parts[1]
                "MODE" -> mode = parts[1]
                "HOOK" -> hook = parts[1]
            }
        }
        return PayloadStatus(status, failed, libSize, mode, hook, raw)
    }

    /** Arti exit code injector dalam bahasa manusia. */
    fun failureReason(exitCode: Int): String = when (exitCode) {
        0 -> "OK"
        2 -> "argumen injector salah (bug aplikasi)"
        3 -> "libdl/libc tidak ketemu di proses injector sendiri"
        4 -> "ptrace attach ditolak — root tidak aktif / SELinux menolak"
        5 -> "libc/libdl tidak ketemu di proses game (layout maps aneh)"
        6 -> "alokasi memori di proses game gagal"
        7 -> "tulis path payload ke memori game gagal"
        8 -> "panggilan dlopen di game tidak selesai (timeout; bila tetap gagal, salin diagnosis)"
        9 -> "dlopen payload ditolak — baca baris dlerror di diagnosis (alasan linker yang sebenarnya)"
        127 -> "injector tidak ditemukan / tidak bisa dieksekusi (command not found — baca baris 'injector:' di diagnosis)"
        else -> "gagal tak dikenal (exit=$exitCode)"
    }

    /** pidof saja (pola TR — tanpa scan /proc fallback: scan itu
     *  memakan waktu beberapa detik dan menahan SELURUH alur sebelum
     *  game sempat diluncurkan; bukti tes v1.0.1). */
    fun findPid(processName: String): Int? {
        val pid = RootUtil.pidOf(processName)
        return if (pid > 0) pid else null
    }

    /** Apakah lib sudah termuat di proses game (poll /proc/<pid>/maps). */
    fun isLibLoaded(pid: Int, libName: String): Boolean {
        return RootUtil.mapsContains(pid, libName)
    }

    /** Anti-double-load: true bila payload sudah ter-map di proses game. */
    fun isPayloadLoaded(pid: Int): Boolean = isLibLoaded(pid, "payload\\.so")

    /** Tunggu game SIAP sebelum ptrace attach (pola TR verbatim):
     *  libil2cpp.so ter-map (maks 20 dtk) + jeda settle 3 dtk biar main
     *  thread keluar dari rantai dlopen Unity. Tanpa ini remote-dlopen
     *  bisa mendarat saat main thread masih memegang linker lock ->
     *  dlopen bersarang deadlock (pelajaran TR v2.0/v2.1).
     *  Bila lib belum ter-map setelah 20 dtk, LANJUT — payload menunggu
     *  sendiri; hanya PID mati/berubah yang membatalkan.
     *  @return false bila PID mati/berubah selama menunggu. */
    fun waitForGameReady(pid: Int, log: (String) -> Unit): Boolean {
        log("Tunggu libil2cpp.so dimuat (maks 20 dtk)...")
        var mapped = false
        for (i in 0 until 20) {
            if (RootUtil.mapsContains(pid, TARGET_LIB)) {
                mapped = true
                break
            }
            if (!RootUtil.pidAlive(pid)) {
                log("PID game mati saat menunggu lib — batal.")
                return false
            }
            Thread.sleep(1000)
        }
        if (!mapped) {
            log("libil2cpp.so belum ter-map setelah 20 dtk — lanjut, payload akan menunggu sendiri.")
        } else {
            log("libil2cpp.so ter-map — jeda 3 dtk biar Unity selesai load...")
            try {
                Thread.sleep(3000)
            } catch (_: InterruptedException) {
            }
            if (!RootUtil.pidAlive(pid)) {
                log("PID game mati saat jeda — batal.")
                return false
            }
        }
        return true
    }

    /** Buka game via root (pola Traffic Rider verbatim): resolve launcher
     *  activity lalu `am start -n`; fallback `monkey` bila resolve gagal.
     *  Poll pidOf maks 20 detik — return true HANYA bila PID game
     *  benar-benar muncul (v1.0.1 return true langsung setelah `am
     *  start` tanpa verifikasi, jadi kegagalan launch tak ketahuan).
     *  Tanpa fallback intent aplikasi: startActivity dari thread
     *  background bisa diblokir pembatasan background-activity-launch
     *  di Android 16, sementara `am start` via root selalu lolos. */
    fun launchGame(log: (String) -> Unit): Boolean {
        val safe = TARGET_PACKAGE.replace(Regex("[^a-zA-Z0-9_.]"), "")
        if (safe.isEmpty()) return false
        val resolve = RootUtil.su(
            "cmd package resolve-activity --brief -a android.intent.action.MAIN " +
                "-c android.intent.category.LAUNCHER $safe | tail -1"
        )
        val comp = resolve.output.trim()
        if (resolve.exitCode == 0 && comp.contains("/")) {
            log("Buka game: $comp")
            RootUtil.su("am start -n $comp")
        } else {
            log("Resolve activity gagal — pakai monkey.")
            RootUtil.su("monkey -p $safe -c android.intent.category.LAUNCHER 1")
        }
        for (i in 0 until 20) {
            val pid = RootUtil.pidOf(safe)
            if (pid > 0) {
                log("Game jalan (pid $pid).")
                return true
            }
            Thread.sleep(1000)
        }
        return false
    }

    /** Bawa game ke depan (tanpa inject) — pola Traffic Rider verbatim:
     *  resolve launcher activity lalu `am start -n`. Dipakai alur
     *  LUNCURKAN setelah inject sukses agar user langsung mendarat di
     *  game dengan overlay tampil. */
    fun bringGameToFront() {
        val safe = TARGET_PACKAGE.replace(Regex("[^a-zA-Z0-9_.]"), "")
        if (safe.isEmpty()) return
        val resolve = RootUtil.su(
            "cmd package resolve-activity --brief -a android.intent.action.MAIN " +
                "-c android.intent.category.LAUNCHER $safe | tail -1"
        )
        val comp = resolve.output.trim()
        if (resolve.exitCode == 0 && comp.contains("/")) {
            RootUtil.su("am start -n $comp")
        }
    }

    /** Tulis config toggle ke cache game via root (dibaca payload tiap 500 ms).
     *  Pola TR (printf, bukan heredoc): satu perintah `su -c`, tanpa
     *  baris-baris heredoc yang harus lolos lewat shell persisten. */
    fun writeConfig(configJson: String): Boolean {
        val esc = configJson.replace("'", "'\"'\"'")
        val r = RootUtil.su(
            "D='$GAME_CACHE_DIR'; mkdir -p \"\$D\" && " +
                "printf '%s' '$esc' > '$CONFIG_PATH' && chmod 644 '$CONFIG_PATH'"
        )
        return r.exitCode == 0
    }

    /** Baca data ESP (JSON) dari cache game via root. "" bila tak ada. */
    fun readEspJson(): String {
        val r = RootUtil.su("cat '$ESP_PATH' 2>/dev/null")
        return r.output.trim()
    }

    /**
     * Salin payload ke lokasi yang BISA di-dlopen proses game.
     * Utama: cache game sendiri (chown ke UID game + samakan konteks
     * SELinux + chmod 755). Cadangan: /data/local/tmp.
     */
    private fun stagePayload(context: Context): String? {
        // Sumber payload: lib dir bila ada, bila tidak ekstrak dari APK
        // (jaring pengaman yang sama seperti injector — v1.0).
        val src = resolveNativeLib(context, PAYLOAD_LIB) ?: return null
        val gameStage = RootUtil.su(
            "D='$GAME_CACHE_DIR'; " +
            "mkdir -p \"\$D\" && cp '$src' '$PAYLOAD_GAME' && " +
            "U=\$(stat -c %u '$GAME_DATA_DIR' 2>/dev/null); " +
            "if [ -n \"\$U\" ]; then chown \"\$U\" '$PAYLOAD_GAME' 2>/dev/null; fi; " +
            "chmod 755 '$PAYLOAD_GAME' && " +
            "chcon --reference=\"\$D\" '$PAYLOAD_GAME' 2>/dev/null; " +
            "ls -l '$PAYLOAD_GAME'"
        )
        if (gameStage.exitCode == 0) return PAYLOAD_GAME
        val r = RootUtil.su("cp '$src' '$PAYLOAD_TMP' && chmod 755 '$PAYLOAD_TMP'")
        return if (r.exitCode == 0) PAYLOAD_TMP else null
    }

    /**
     * Inject + kumpulkan diagnosis. Retry s.d. 3x (cache game → native
     * lib → cache game), cek maps antar percobaan — dlopen percobaan
     * sebelumnya bisa selesai TERLAMBAT, dan dlopen kedua ke payload
     * yang sudah ter-map = crash.
     */
    fun inject(context: Context, pid: Int): InjectResult {
        // Pre-flight (pelajaran v1.0): injector HARUS ada sebagai file
        // sebelum dieksekusi. Bila tidak ada, JANGAN panggil 3x buta —
        // kembalikan 127 dengan diagnosis yang menyebut path persisnya.
        val injector = resolveNativeLib(context, INJECTOR_LIB)
        if (injector == null) {
            val ls = RootUtil.su(
                "ls -l '${context.applicationInfo.nativeLibraryDir}' 2>&1"
            ).output.trim()
            return InjectResult(
                127, "",
                "libinjector.so TIDAK ditemukan di nativeLibraryDir dan gagal diekstrak dari APK.\n" +
                    "nativeLibraryDir: ${context.applicationInfo.nativeLibraryDir}\n$ls\n" +
                    "Uninstall aplikasi ini lalu install ulang APK v1.0.1."
            )
        }
        RootUtil.su("chmod 755 '$injector'")
        if (isPayloadLoaded(pid)) {
            return InjectResult(
                0, "",
                "payload sudah ter-load di pid $pid — lewati dlopen, langsung tunggu status"
            )
        }
        val staged = stagePayload(context)
        val nativePayload = resolveNativeLib(context, PAYLOAD_LIB) ?: payloadPath(context)
        val primary = staged ?: nativePayload
        val attempts = arrayOf(primary, nativePayload, primary)
        var last: InjectResult? = null
        val allDiag = StringBuilder("injector: $injector")
        for ((i, path) in attempts.withIndex()) {
            if (i > 0) {
                Thread.sleep(1000)
                if (isPayloadLoaded(pid)) {
                    val d = "payload ter-load (percobaan sebelumnya) — stop retry\n" +
                        (last?.diag ?: "")
                    val full = if (allDiag.isNotEmpty()) "$allDiag\n$d" else d
                    return InjectResult(0, "", full)
                }
            }
            // Timeout 30 dtk (v1.0.3): injector bisa menunggu dlopen
            // lambat s.d. ~25 dtk sebelum menyerah — timeout shell 15 dtk
            // (default) akan membunuhnya di tengah tunggu dan
            // meninggalkan thread game kembali ke brk tanpa tracer.
            val r = RootUtil.su("'$injector' $pid $path", 30)
            var diag = r.output.trim()
            if (diag.isEmpty()) {
                val log = RootUtil.su("cat '$INJECT_LOG' 2>/dev/null")
                diag = log.output.trim()
            }
            val ctor = readCtorLog()
            if (ctor.isNotEmpty()) diag = "$diag\nctor log:\n$ctor"
            if (diag.isNotEmpty()) diag = "percobaan ${i + 1}/3 — payload path dipakai: $path\n$diag"
            if (r.exitCode != 0) {
                val failLine = "percobaan ${i + 1}/3 GAGAL: exit=${r.exitCode} — ${failureReason(r.exitCode)}"
                diag = if (diag.isNotEmpty()) "$diag\n$failLine" else failLine
            }
            if (allDiag.isNotEmpty()) allDiag.append("\n")
            allDiag.append(diag)
            last = InjectResult(r.exitCode, r.output, diag)
            if (r.exitCode == 0) return InjectResult(r.exitCode, r.output, allDiag.toString())
        }
        return InjectResult(
            last?.exitCode ?: 1, last?.output ?: "",
            if (allDiag.isNotEmpty()) allDiag.toString() else "inject gagal tanpa percobaan"
        )
    }
}
