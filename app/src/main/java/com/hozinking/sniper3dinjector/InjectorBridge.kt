package com.hozinking.sniper3dinjector

import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import java.io.File

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
        val r = RootShell.exec(
            "cat '$PAYLOAD_STATUS_TMP' 2>/dev/null || cat '$PAYLOAD_STATUS_CACHE' 2>/dev/null"
        )
        return r.output.trim()
    }

    /** Baca ctor log payload. "" bila tak ada. */
    fun readCtorLog(): String {
        val r = RootShell.exec("cat '$CTOR_LOG_CACHE' 2>/dev/null")
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

    fun ensureExecutable(context: Context): Boolean {
        val result = RootShell.exec("chmod 755 '${injectorPath(context)}'")
        return result.exitCode == 0
    }

    /** pidof dulu; bila kosong, scan /proc/<pid>/cmdline via su. */
    fun findPid(processName: String): Int? {
        val r = RootShell.exec("pidof $processName")
        val pid = r.output.trim().split(Regex("\\s+")).firstOrNull()?.toIntOrNull()
        if (r.exitCode == 0 && pid != null) return pid
        val safe = processName.replace(".", "[.]")
        val scan = "for p in /proc/[0-9]*/cmdline; do " +
            "if grep -a -q '^$safe' \"\$p\" 2>/dev/null; then " +
            "echo \"\$p\" | cut -d/ -f3; fi; done"
        val r2 = RootShell.exec(scan)
        return r2.output.trim().split(Regex("\\s+")).firstOrNull()?.toIntOrNull()
    }

    /** Apakah lib sudah termuat di proses game (poll /proc/<pid>/maps). */
    fun isLibLoaded(pid: Int, libName: String): Boolean {
        val r = RootShell.exec("grep -q '$libName' /proc/$pid/maps")
        return r.exitCode == 0
    }

    /** Anti-double-load: true bila payload sudah ter-map di proses game. */
    fun isPayloadLoaded(pid: Int): Boolean = isLibLoaded(pid, "payload\\.so")

    /** Luncurkan game (intent dulu, fallback monkey via su). */
    fun launchGame(context: Context): Boolean {
        try {
            val intent = context.packageManager.getLaunchIntentForPackage(TARGET_PACKAGE)
            if (intent != null) {
                intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                context.startActivity(intent)
                return true
            }
        } catch (e: Exception) {
            // jatuh ke monkey di bawah
        }
        val r = RootShell.exec(
            "monkey -p $TARGET_PACKAGE -c android.intent.category.LAUNCHER 1"
        )
        return r.exitCode == 0
    }

    /** Tulis config toggle ke cache game via root (dibaca payload tiap 500 ms). */
    fun writeConfig(configJson: String): Boolean {
        val r = RootShell.exec(
            "D='$GAME_CACHE_DIR'; mkdir -p \"\$D\" && " +
            "cat > '$CONFIG_PATH' <<'HOZINCFG'\n$configJson\nHOZINCFG\n" +
            "chmod 644 '$CONFIG_PATH'"
        )
        return r.exitCode == 0
    }

    /** Baca data ESP (JSON) dari cache game via root. "" bila tak ada. */
    fun readEspJson(): String {
        val r = RootShell.exec("cat '$ESP_PATH' 2>/dev/null")
        return r.output.trim()
    }

    /**
     * Salin payload ke lokasi yang BISA di-dlopen proses game.
     * Utama: cache game sendiri (chown ke UID game + samakan konteks
     * SELinux + chmod 755). Cadangan: /data/local/tmp.
     */
    private fun stagePayload(context: Context): String? {
        val src = payloadPath(context)
        val gameStage = RootShell.exec(
            "D='$GAME_CACHE_DIR'; " +
            "mkdir -p \"\$D\" && cp '$src' '$PAYLOAD_GAME' && " +
            "U=\$(stat -c %u '$GAME_DATA_DIR' 2>/dev/null); " +
            "if [ -n \"\$U\" ]; then chown \"\$U\" '$PAYLOAD_GAME' 2>/dev/null; fi; " +
            "chmod 755 '$PAYLOAD_GAME' && " +
            "chcon --reference=\"\$D\" '$PAYLOAD_GAME' 2>/dev/null; " +
            "ls -l '$PAYLOAD_GAME'"
        )
        if (gameStage.exitCode == 0) return PAYLOAD_GAME
        val r = RootShell.exec("cp '$src' '$PAYLOAD_TMP' && chmod 755 '$PAYLOAD_TMP'")
        return if (r.exitCode == 0) PAYLOAD_TMP else null
    }

    /**
     * Inject + kumpulkan diagnosis. Retry s.d. 3x (cache game → native
     * lib → cache game), cek maps antar percobaan — dlopen percobaan
     * sebelumnya bisa selesai TERLAMBAT, dan dlopen kedua ke payload
     * yang sudah ter-map = crash.
     */
    fun inject(context: Context, pid: Int): InjectResult {
        ensureExecutable(context)
        if (isPayloadLoaded(pid)) {
            return InjectResult(
                0, "",
                "payload sudah ter-load di pid $pid — lewati dlopen, langsung tunggu status"
            )
        }
        val staged = stagePayload(context)
        val primary = staged ?: payloadPath(context)
        val attempts = arrayOf(primary, payloadPath(context), primary)
        var last: InjectResult? = null
        val allDiag = StringBuilder()
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
            val r = RootShell.exec("${injectorPath(context)} $pid $path")
            var diag = r.output.trim()
            if (diag.isEmpty()) {
                val log = RootShell.exec("cat '$INJECT_LOG' 2>/dev/null")
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
        else -> "gagal tak dikenal (exit=$exitCode)"
    }
}
