#pragma once

#include <cstdint>
#include <sys/types.h>
#include <vector>

namespace injector {

// Thin wrapper around a single ptrace-attached remote process. Everything
// here assumes arm64-v8a: register layout, calling convention (first eight
// integer args in x0-x7, return value in x0) and the breakpoint trampoline
// used to detect when a remote call has returned. Not safe to share across
// threads.
class RemoteProcess {
public:
    bool attach(pid_t pid);
    bool detach();

    bool read_mem(uintptr_t addr, void* buf, size_t len) const;
    bool write_mem(uintptr_t addr, const void* buf, size_t len) const;

    // Calls func_addr(args...) inside the attached process and returns the
    // value left in x0 through result. Returns false (result left
    // untouched) if the call could not be completed.
    //
    // trampoline (v1.0.3): alamat scratch PROT_EXEC di proses target
    // yang dipakai sebagai return marker (LR + instruksi `brk #0`).
    // Bila 0 (hanya untuk bootstrap mmap pertama — scratch belum ada),
    // marker lama dipakai: `brk` ditanam di saved_regs.pc. JANGAN
    // pernah pakai marker lama untuk panggilan LAMA seperti dlopen:
    // PC main thread hampir pasti berada di fungsi library bersama
    // (mis. libc __epoll_pwait) dan thread lain yang tak di-suspend
    // bisa mengeksekusi instruksi yang sama → SIGTRAP → tombstone.
    //
    // max_wait_ms: budget tunggu breakpoint. Setelah budget habis,
    // call TETAP menunggu (tanpa me-restore register) sampai
    // breakpoint kena atau proses mati, dengan batas total 25 dtk.
    // Memotong dlopen di tengah jalan dan me-restore register secara
    // buta meninggalkan linker-lock rusak → crash.
    bool call(uintptr_t func_addr, const std::vector<uint64_t>& args, uint64_t& result,
              uintptr_t trampoline = 0, int max_wait_ms = 2000);

private:
    pid_t pid_ = -1;
    int mem_fd_ = -1;
    bool attached_ = false;
};

}  // namespace injector
