#include "unix_system.h"

#include <dirent.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

#include <date/date.h>
#include <fmt/chrono.h>
#include <fmt/core.h>

namespace pivid {

namespace {

inline ErrnoOr<int> run_sys(std::function<int()> f) {
    ErrnoOr<int> ret;
    ret.value = f();
    ret.err = errno;
    if (ret.value < 0 && ret.err == EINTR) return run_sys(f);
    return (ret.value >= 0) ? ErrnoOr<int>{0, ret.value} : ret;
}

class FileDescriptorDef : public FileDescriptor {
  public:
    FileDescriptorDef(int fd) : fd(fd) {}
    virtual ~FileDescriptorDef() final { ::close(fd); }
    virtual int raw_fd() const final { return fd; }

    virtual ErrnoOr<int> read(void* buf, size_t len) final {
        return run_sys([&] {return ::read(fd, buf, len);});
    }

    virtual ErrnoOr<int> write(void const* buf, size_t len) final {
        return run_sys([&] {return ::write(fd, buf, len);});
    }

    virtual ErrnoOr<int> ioctl(uint32_t nr, void* buf) final {
        return run_sys([&] {return ::ioctl(fd, nr, buf);});
    }

    virtual ErrnoOr<std::shared_ptr<void>> mmap(
        size_t len, int prot, int flags, off_t off
    ) final {
        void* const mem = ::mmap(nullptr, len, prot, flags, fd, off);
        if (mem == MAP_FAILED) return {errno, {}};
        return {0, {mem, [len](void* m) {::munmap(m, len);}}};
    }

  private:
    int fd = -1;
};

class ThreadSignalDef : public ThreadSignal {
  public:
    virtual void set() final {
        std::scoped_lock<std::mutex> lock{mutex};
        if (!signal_flag) {
            signal_flag = true;
            condvar.notify_one();
        }
    }

    virtual void wait() final {
        std::unique_lock<std::mutex> lock{mutex};
        while (!signal_flag)
            condvar.wait(lock);
        signal_flag = false;
    }

    virtual bool wait_until(double t) final {
        using namespace std::chrono;
        duration<double> const double_d{t};
        auto const system_d = duration_cast<system_clock::duration>(double_d);
        system_clock::time_point system_t(system_d);
        std::unique_lock<std::mutex> lock{mutex};
        while (!signal_flag) {
            if (condvar.wait_until(lock, system_t) == std::cv_status::timeout)
                return false;
        }
        signal_flag = false;
        return true;
    }

    virtual bool wait_for(double t) final {
        using namespace std::chrono;
        auto const steady_t = steady_clock::now() + duration<double>{t};
        std::unique_lock<std::mutex> lock{mutex};
        while (!signal_flag) {
            if (condvar.wait_until(lock, steady_t) == std::cv_status::timeout)
                return false;
        }
        signal_flag = false;
        return true;
    }

  private:
    std::mutex mutable mutex;
    std::condition_variable condvar;
    bool signal_flag = false;
};

class UnixSystemDef : public UnixSystem {
  public:
    virtual double system_time() const final {
        auto const now = std::chrono::system_clock::now();
        return std::chrono::duration<double>{now.time_since_epoch()}.count();
    }

    virtual std::unique_ptr<ThreadSignal> make_signal() const final {
        return std::make_unique<ThreadSignalDef>();
    }

    virtual ErrnoOr<std::vector<std::string>> ls(
        std::string const& dir
    ) const final {
        std::unique_ptr<DIR, int (*)(DIR*)> dp(opendir(dir.c_str()), closedir);
        if (!dp) return {errno, {}};

        ErrnoOr<std::vector<std::string>> ret;
        while (dirent* ent = readdir(dp.get()))
            ret.value.push_back(ent->d_name);
        std::sort(ret.value.begin(), ret.value.end());
        return ret;
    }

    virtual ErrnoOr<struct stat> stat(std::string const& path) const final {
        ErrnoOr<struct stat> ret;
        ret.err = run_sys([&] {return ::stat(path.c_str(), &ret.value);}).err;
        return ret;
    }

    virtual ErrnoOr<std::string> realpath(std::string const& path) const final {
        char buf[PATH_MAX];
        if (!::realpath(path.c_str(), buf)) return {errno, {}};
        return {0, buf};
    }

    virtual ErrnoOr<std::unique_ptr<FileDescriptor>> open(
        std::string const& path, int flags, mode_t mode
    ) final {
        auto const r = run_sys([&] {return ::open(path.c_str(), flags, mode);});
        if (r.value < 0) return {r.err ? r.err : EBADF, {}};
        return {0, adopt(r.value)};
    }

    virtual std::unique_ptr<FileDescriptor> adopt(int raw_fd) final {
        return std::make_unique<FileDescriptorDef>(raw_fd);
    }

    virtual ErrnoOr<pid_t> spawn(
        std::string const& command,
        std::vector<std::string> const& argv,
        posix_spawn_file_actions_t const* actions,
        posix_spawnattr_t const* attr,
        std::optional<std::vector<std::string>> const& envp
    ) final {
        auto const c_vector = [](std::vector<std::string> const& vec) {
            std::vector<char*> ret;
            for (auto& s : vec) ret.push_back(const_cast<char*>(s.c_str()));
            return ret;
        };

        pid_t pid = 0;
        auto const r = run_sys([&] {
            return ::posix_spawnp(
                &pid, command.c_str(), actions, attr,
                c_vector(argv).data(),
                envp ? c_vector(*envp).data() : environ
            );
        });
        return {r.err, pid};
    }

    virtual ErrnoOr<siginfo_t> wait(idtype_t idtype, id_t id, int flags) final {
        siginfo_t s = {};
        auto const r = run_sys([&] { return ::waitid(idtype, id, &s, flags); });
        return {r.err, s};
    }
};

}  // namespace

std::shared_ptr<UnixSystem> global_system() {
    static const auto system = std::make_shared<UnixSystemDef>();
    return system;
}

double parse_time(std::string const& s) {
    size_t end;
    double const d = std::stod(s, &end);
    if (!s.empty() && end >= s.size())
        return d;

    std::chrono::system_clock::time_point t = {};
    for (auto f : {
        "%FT%H:%M:%20SZ", "%FT%H:%M:%20S%Ez", "%F %H:%M:%S%Ez", "%F %H:%M:%S"
    }) {
        std::istringstream is{s};
        date::from_stream(is, f, t);
        if (!is.fail())
            return std::chrono::duration<double>{t.time_since_epoch()}.count();
    }

    throw std::invalid_argument("Bad date: \"" + s + "\"");
}

static std::chrono::system_clock::time_point as_time_point(double t) {
    using namespace std::chrono;
    duration<double> const double_d{t};
    auto const system_d = duration_cast<system_clock::duration>(double_d);
    return system_clock::time_point{system_d};
}

std::string format_date_time(double t) {
    int const millis = std::fmod(t, 1.0) * 1000;
    return fmt::format("{0:%F %R:%S.}{1:03d}{0:%z}", as_time_point(t), millis);
}

std::string abbrev_time(double t) {
    int const millis = std::fmod(t, 1.0) * 1000;
    return fmt::format("{0:%R:%S.}{1:03d}", as_time_point(t), millis);
}

}  // namespace pivid
