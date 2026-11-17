#include "remote_call_arm64.h"

#include <android/log.h>
#include <asm/ptrace.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <elf.h>
#include <fcntl.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define LOG_TAG "HozinSniper3D"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

namespace injector {

namespace {
constexpr int kWaitRetryUsec = 10000;   // 10 ms per poll waitpid
constexpr int kDefaultWaitMs = 2000;    // budget default (mmap/dlerror)
constexpr int kMaxTotalWaitMs = 25000;  // batas total tunggu — di bawah timeout shell 30 dtk
constexpr uint32_t kBrk0 = 0xD4200000;  // "brk #0" on arm64
}  // namespace

bool RemoteProcess::attach(pid_t pid) {
    if (ptrace(PTRACE_ATTACH, pid, nullptr, nullptr) != 0) {
        LOGE("PTRACE_ATTACH failed for pid %d: %s", pid, std::strerror(errno));
        return false;
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0 || !WIFSTOPPED(status)) {
        LOGE("waitpid after attach failed for pid %d", pid);
        ptrace(PTRACE_DETACH, pid, nullptr, nullptr);
        return false;
    }

    char mem_path[32];
    std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem", pid);
    mem_fd_ = open(mem_path, O_RDWR);
    if (mem_fd_ < 0) {
        LOGE("open %s failed: %s", mem_path, std::strerror(errno));
        ptrace(PTRACE_DETACH, pid, nullptr, nullptr);
        return false;
    }

    pid_ = pid;
    attached_ = true;
    return true;
}

bool RemoteProcess::detach() {
    if (!attached_) {
        return true;
    }
    if (mem_fd_ >= 0) {
        close(mem_fd_);
        mem_fd_ = -1;
    }
    bool ok = ptrace(PTRACE_DETACH, pid_, nullptr, nullptr) == 0;
    attached_ = false;
    return ok;
}

bool RemoteProcess::read_mem(uintptr_t addr, void* buf, size_t len) const {
    ssize_t n = pread(mem_fd_, buf, len, static_cast<off_t>(addr));
    return n == static_cast<ssize_t>(len);
}

bool RemoteProcess::write_mem(uintptr_t addr, const void* buf, size_t len) const {
    ssize_t n = pwrite(mem_fd_, buf, len, static_cast<off_t>(addr));
    return n == static_cast<ssize_t>(len);
}

bool RemoteProcess::call(uintptr_t func_addr, const std::vector<uint64_t>& args,
                         uint64_t& result, uintptr_t trampoline, int max_wait_ms) {
    if (!attached_ || args.size() > 8) {
        return false;
    }
    if (max_wait_ms <= 0) {
        max_wait_ms = kDefaultWaitMs;
    }

    struct user_pt_regs saved_regs {};
    struct iovec iov{&saved_regs, sizeof(saved_regs)};
    if (ptrace(PTRACE_GETREGSET, pid_, reinterpret_cast<void*>(NT_PRSTATUS), &iov) != 0) {
        LOGE("PTRACE_GETREGSET failed: %s", std::strerror(errno));
        return false;
    }

    struct user_pt_regs work_regs = saved_regs;
    for (size_t i = 0; i < args.size(); ++i) {
        work_regs.regs[i] = args[i];
    }

    // Return marker: trampolin scratch (milik kita, tak ada thread lain
    // yang menyentuhnya) bila diberi; kalau tidak — hanya bootstrap mmap
    // pertama — PC main thread, dengan instruksi aslinya dipulihkan.
    const bool use_trampoline = trampoline != 0;
    uintptr_t return_marker = use_trampoline ? trampoline : saved_regs.pc;
    work_regs.pc = func_addr;
    work_regs.regs[30] = return_marker;      // x30 / LR
    work_regs.sp = saved_regs.sp & ~0xFULL;  // AAPCS64 16-byte alignment

    bool planted = false;
    uint32_t original_insn = 0;
    if (use_trampoline) {
        // Scratch milik kita: tak ada instruksi asli yang perlu dipulihkan.
        if (!write_mem(return_marker, &kBrk0, sizeof(kBrk0))) {
            LOGE("failed to plant breakpoint at trampoline 0x%lx", return_marker);
            return false;
        }
        planted = true;
    } else {
        if (!read_mem(return_marker, &original_insn, sizeof(original_insn))) {
            LOGE("failed to read original instruction at return marker 0x%lx", return_marker);
            return false;
        }
        if (!write_mem(return_marker, &kBrk0, sizeof(kBrk0))) {
            LOGE("failed to plant breakpoint at 0x%lx", return_marker);
            return false;
        }
        planted = true;
    }

    struct iovec set_iov{&work_regs, sizeof(work_regs)};
    if (ptrace(PTRACE_SETREGSET, pid_, reinterpret_cast<void*>(NT_PRSTATUS), &set_iov) != 0) {
        LOGE("PTRACE_SETREGSET failed: %s", std::strerror(errno));
        if (!use_trampoline && planted) {
            write_mem(return_marker, &original_insn, sizeof(original_insn));
        }
        return false;
    }

    // PTRACE_CONT sekali, lalu poll waitpid — JANGAN CONT di tiap
    // iterasi (v1.0.2: CONT pada thread yang sedang jalan gagal ESRCH
    // dan memotong loop, jadi budget efektifnya hanya ~10-20 ms, bukan
    // ~2 dtk). Stop yang bukan breakpoint kita (mis. SIGSTOP) di-CONT
    // lagi dari dalam loop.
    if (ptrace(PTRACE_CONT, pid_, nullptr, nullptr) != 0) {
        LOGE("PTRACE_CONT failed: %s", std::strerror(errno));
        if (!use_trampoline && planted) {
            write_mem(return_marker, &original_insn, sizeof(original_insn));
        }
        struct iovec restore_iov{&saved_regs, sizeof(saved_regs)};
        ptrace(PTRACE_SETREGSET, pid_, reinterpret_cast<void*>(NT_PRSTATUS), &restore_iov);
        return false;
    }

    struct timespec start_ts {};
    clock_gettime(CLOCK_MONOTONIC, &start_ts);
    auto elapsed_ms = [&]() -> long {
        struct timespec now {};
        clock_gettime(CLOCK_MONOTONIC, &now);
        return (now.tv_sec - start_ts.tv_sec) * 1000L +
               (now.tv_nsec - start_ts.tv_nsec) / 1000000L;
    };

    bool hit = false;
    bool process_dead = false;
    bool gave_up = false;
    bool warned_over_budget = false;
    while (!hit && !process_dead && !gave_up) {
        int status = 0;
        pid_t w = waitpid(pid_, &status, WNOHANG);
        if (w == 0) {
            if (elapsed_ms() >= kMaxTotalWaitMs) {
                LOGE("remote call ke 0x%lx tak selesai setelah %d ms — pulihkan register "
                     "sebagai jalan terakhir (proses hampir pasti macet)",
                     func_addr, kMaxTotalWaitMs);
                gave_up = true;
                break;
            }
            if (elapsed_ms() >= max_wait_ms && !warned_over_budget) {
                warned_over_budget = true;
                LOGI("remote call ke 0x%lx melewati budget %d ms — tetap menunggu breakpoint "
                     "atau proses mati",
                     func_addr, max_wait_ms);
            }
            usleep(kWaitRetryUsec);
            continue;
        }
        if (w < 0) {
            if (errno == EINTR) continue;
            // ECHILD: proses sudah mati / tak ada lagi anak yang ditunggu.
            process_dead = true;
            break;
        }
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            LOGE("target pid %d mati saat remote call ke 0x%lx (status 0x%x)",
                 pid_, func_addr, status);
            process_dead = true;
            break;
        }
        if (WIFSTOPPED(status) && WSTOPSIG(status) == SIGTRAP) {
            struct user_pt_regs check_regs {};
            struct iovec check_iov{&check_regs, sizeof(check_regs)};
            ptrace(PTRACE_GETREGSET, pid_, reinterpret_cast<void*>(NT_PRSTATUS), &check_iov);
            // arm64 leaves pc pointing at the brk instruction itself on trap.
            if (check_regs.pc == return_marker) {
                hit = true;
                result = check_regs.regs[0];
                break;
            }
            // SIGTRAP lain (bukan breakpoint kita) — lanjutkan thread.
        }
        if (ptrace(PTRACE_CONT, pid_, nullptr, nullptr) != 0) {
            LOGE("PTRACE_CONT (lanjutkan setelah stop) failed: %s", std::strerror(errno));
            break;
        }
    }

    // Pulihkan instruksi asli hanya untuk marker lama; scratch trampolin
    // milik kita dan boleh tetap berisi brk.
    if (!use_trampoline && planted) {
        write_mem(return_marker, &original_insn, sizeof(original_insn));
    }

    // Proses mati → tak ada yang bisa dipulihkan. Bila menyerah karena
    // batas total, kembalikan register tersimpan supaya thread lanjut
    // dari keadaan semula (jalan terakhir, tercatat di log di atas).
    if (!process_dead) {
        struct iovec restore_iov{&saved_regs, sizeof(saved_regs)};
        ptrace(PTRACE_SETREGSET, pid_, reinterpret_cast<void*>(NT_PRSTATUS), &restore_iov);
    }

    if (!hit) {
        LOGE("remote call to 0x%lx tidak selesai (proses mati=%d)", func_addr,
             static_cast<int>(process_dead));
    }
    return hit;
}

}  // namespace injector
