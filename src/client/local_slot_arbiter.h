#pragma once

#include <semaphore>
#include <string>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace suco {

/**
 * @brief Arbitrates the build machine's local compile slots.
 *
 * Why this exists: cold builds were ~1.15x behind icecc although the grid ran at
 * ~100% slot efficiency — icecc simply ALSO compiles on the client (measured 667%
 * client CPU vs SUCO's 191%). Using the idle client cores needs a slot budget that
 * is shared across ALL concurrently running suco-cl++ processes: ninja -j32 spawns
 * 32 of them, and a per-process budget of "cores-2" oversubscribed the machine 20x
 * (measured: 21.6s vs 7.9s on a 60-file build). The daemon solves this in-process
 * with one shared semaphore; standalone mode needs the same guarantee across
 * processes.
 */
class LocalSlotArbiter {
public:
    virtual ~LocalSlotArbiter() = default;
    virtual bool try_acquire() = 0;
    virtual void acquire() = 0;
    virtual void release() = 0;
};

/** @brief In-process arbiter over the daemon's shared counting semaphore. */
class SemaphoreSlotArbiter final : public LocalSlotArbiter {
public:
    explicit SemaphoreSlotArbiter(std::counting_semaphore<1024>* sem) : sem_(sem) {}
    bool try_acquire() override { return sem_->try_acquire(); }
    void acquire() override { sem_->acquire(); }
    void release() override { sem_->release(); }

private:
    std::counting_semaphore<1024>* sem_;
};

#ifndef _WIN32
/**
 * @brief Cross-process arbiter: N flock()ed slot files shared by every suco-cl++
 *        on this machine.
 *
 * flock over a named POSIX semaphore on purpose: the kernel releases a flock when
 * the holding process dies, so a crashed/killed compile can never leak a slot. A
 * sem_open() semaphore would stay drained forever and silently degrade every
 * later build to grid-only.
 */
class FlockSlotArbiter final : public LocalSlotArbiter {
public:
    explicit FlockSlotArbiter(int slots) : slots_(slots) {
        dir_ = "/tmp/suco-local-slots-" + std::to_string(::getuid());
        ::mkdir(dir_.c_str(), 0700);
    }

    bool try_acquire() override {
        for (int i = 0; i < slots_; ++i) {
            std::string p = dir_ + "/slot_" + std::to_string(i);
            int fd = ::open(p.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
            if (fd < 0) continue;
            if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
                held().push_back(fd);
                return true;
            }
            ::close(fd);
        }
        return false;
    }

    void acquire() override {
        // Jobs that MUST run locally (C++20 modules without CMIs, __DATE__/__TIME__)
        // block here. Cap the wait: proceeding oversubscribed after 120s beats
        // hanging a build forever on a wedged slot holder.
        for (int waited_ms = 0; waited_ms < 120000; waited_ms += 25) {
            if (try_acquire()) return;
            ::usleep(25 * 1000);
        }
        held().push_back(-1);   // sentinel: ran without a slot
    }

    void release() override {
        if (held().empty()) return;
        int fd = held().back();
        held().pop_back();
        if (fd >= 0) ::close(fd);   // closing the fd drops the flock
    }

private:
    // acquire/release pair up within one thread, so a thread-local stack suffices.
    static std::vector<int>& held() {
        static thread_local std::vector<int> h;
        return h;
    }

    int slots_;
    std::string dir_;
};
#endif

} // namespace suco
