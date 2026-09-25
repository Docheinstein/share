// share - dead simple file sharing between peers on a local network.
//
//   share recv [name]              wait for files, saved into the current directory
//   share send <name> <path>...    send files/directories to the receiver called <name>
//   share list                     list the receivers visible on the network, with their folders
//   share send                     same as 'share list'
//
// Discovery: the sender broadcasts a UDP query on port 47001, every receiver
// answers with its name and TCP port. The transfer is a single TCP stream.

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <io.h>
#include <shellapi.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cctype>
#include <csignal>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

constexpr const char* kVersion = "1.0.0";
constexpr uint16_t kDiscoveryPort = 47001;
constexpr uint16_t kPreferredTcpPort = 47002;
constexpr const char kMagic[4] = {'S', 'H', 'R', '1'};
constexpr const char* kQuery = "SHARE1 Q";
// Newer query: also asks for the receiver's folder. Older receivers still
// answer it (they only check the kQuery prefix), just without the folder.
constexpr const char* kQueryFolder = "SHARE1 Q F";
constexpr const char* kAnswerPrefix = "SHARE1 A ";
constexpr size_t kMaxName = 200;
constexpr size_t kChunk = 1 << 20;

enum EntryType : uint8_t { kFile = 0, kDir = 1 };
enum Reply : uint8_t { kAccept = 'Y', kReject = 'N', kDone = 'K' };

[[noreturn]] void fail(const std::string& msg) { throw std::runtime_error(msg); }

// ---------------------------------------------------------------- platform
//
// Everything that differs between POSIX and Windows lives here: sockets,
// files, network interfaces, Ctrl-C handling and path/argument encoding.
// Paths and names travel on the wire (and through the rest of the program) as
// UTF-8 strings; on POSIX that is just the native byte string.

#ifdef _WIN32
using socket_t = SOCKET;
constexpr socket_t kNoSocket = INVALID_SOCKET;
constexpr int kShutWrite = SD_SEND;
constexpr int kSendFlags = 0;
using io_len_t = int;
using native_file_t = HANDLE;
const native_file_t kNoFile = INVALID_HANDLE_VALUE;

int sock_error() { return WSAGetLastError(); }
bool is_interrupted(int) { return false; }
bool is_timeout(int e) { return e == WSAETIMEDOUT || e == WSAEWOULDBLOCK; }
int file_error() { return static_cast<int>(GetLastError()); }

std::string error_text(int e) {
    std::string s = std::system_category().message(e);
    while (!s.empty() && (std::isspace(static_cast<unsigned char>(s.back())) || s.back() == '.')) s.pop_back();
    return s;
}

std::string utf8(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 1 ? static_cast<size_t>(n - 1) : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::string utf8(const fs::path& p) {
    std::u8string s = p.u8string();
    return {s.begin(), s.end()};
}

std::string utf8_generic(const fs::path& p) {
    std::u8string s = p.generic_u8string();
    return {s.begin(), s.end()};
}

fs::path from_utf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
#else
using socket_t = int;
constexpr socket_t kNoSocket = -1;
constexpr int kShutWrite = SHUT_WR;
constexpr int kSendFlags = MSG_NOSIGNAL;
using io_len_t = size_t;
using native_file_t = int;
constexpr native_file_t kNoFile = -1;

int sock_error() { return errno; }
bool is_interrupted(int e) { return e == EINTR; }
bool is_timeout(int e) { return e == EAGAIN || e == EWOULDBLOCK; }
int file_error() { return errno; }
std::string error_text(int e) { return std::strerror(e); }
std::string utf8(const fs::path& p) { return p.string(); }
std::string utf8_generic(const fs::path& p) { return p.generic_string(); }
fs::path from_utf8(const std::string& s) { return fs::path(s); }
#endif

[[noreturn]] void fail_sys(const std::string& what, int err) { fail(what + ": " + error_text(err)); }
[[noreturn]] void fail_sock(const std::string& what) { fail_sys(what, sock_error()); }

struct Fd {
    socket_t fd = kNoSocket;
    Fd() = default;
    explicit Fd(socket_t f) : fd(f) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& o) noexcept : fd(std::exchange(o.fd, kNoSocket)) {}
    ~Fd() {
        if (!valid()) return;
#ifdef _WIN32
        ::closesocket(fd);
#else
        ::close(fd);
#endif
    }
    bool valid() const { return fd != kNoSocket; }
    operator socket_t() const { return fd; }
};

// Initializes the socket library for the lifetime of the program.
struct NetInit {
#ifdef _WIN32
    NetInit() {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) fail("cannot initialize networking");
    }
    ~NetInit() { WSACleanup(); }
#else
    NetInit() {}
#endif
};

long long sock_send(socket_t s, const void* p, size_t n) {
    return ::send(s, static_cast<const char*>(p), static_cast<io_len_t>(std::min<size_t>(n, 1 << 30)), kSendFlags);
}

long long sock_recv(socket_t s, void* p, size_t n) {
    return ::recv(s, static_cast<char*>(p), static_cast<io_len_t>(std::min<size_t>(n, 1 << 30)), 0);
}

// Like recvfrom(), but a datagram larger than the buffer is truncated on
// Windows too instead of being reported as an error.
long long udp_recvfrom(socket_t s, void* p, size_t n, sockaddr_in& from) {
    socklen_t fl = sizeof from;
    long long r = ::recvfrom(s, static_cast<char*>(p), static_cast<io_len_t>(n), 0, reinterpret_cast<sockaddr*>(&from), &fl);
#ifdef _WIN32
    if (r < 0 && WSAGetLastError() == WSAEMSGSIZE) r = static_cast<long long>(n);
#endif
    return r;
}

long long udp_sendto(socket_t s, const void* p, size_t n, const sockaddr_in& to) {
    return ::sendto(s, static_cast<const char*>(p), static_cast<io_len_t>(n), 0, reinterpret_cast<const sockaddr*>(&to),
                    sizeof to);
}

void set_opt(socket_t s, int level, int opt, int value) {
    setsockopt(s, level, opt, reinterpret_cast<const char*>(&value), sizeof value);
}

socket_t udp_socket() {
    socket_t s = ::socket(AF_INET, SOCK_DGRAM, 0);
#ifdef _WIN32
    // Windows reports ICMP "port unreachable" (e.g. a sender that already went
    // away) as an error on the next recvfrom(); ignore those like POSIX does.
    if (s != kNoSocket) {
        constexpr DWORD kUdpConnReset = _WSAIOW(IOC_VENDOR, 12);  // SIO_UDP_CONNRESET
        BOOL report = FALSE;
        DWORD ret = 0;
        WSAIoctl(s, kUdpConnReset, &report, sizeof report, nullptr, 0, &ret, nullptr, nullptr);
    }
#endif
    return s;
}

// Lets several receivers on the same machine share the discovery port.
void allow_shared_udp_port(socket_t s) {
    set_opt(s, SOL_SOCKET, SO_REUSEADDR, 1);
#ifdef SO_REUSEPORT
    set_opt(s, SOL_SOCKET, SO_REUSEPORT, 1);
#endif
}

// Allows a quick restart on the TCP port, but never binding it twice.
void allow_tcp_port_reuse(socket_t s) {
#ifdef _WIN32
    // On Windows SO_REUSEADDR would let two receivers bind the same port, and
    // a restarted listener can rebind right away anyway.
    set_opt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1);
#else
    set_opt(s, SOL_SOCKET, SO_REUSEADDR, 1);
#endif
}

void set_recv_timeout(socket_t s, int seconds) {
#ifdef _WIN32
    DWORD ms = static_cast<DWORD>(seconds) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof ms);
#else
    timeval tv{seconds, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
}

// True if `s` becomes readable within `timeout_ms`.
bool wait_readable(socket_t s, int timeout_ms) {
#ifdef _WIN32
    WSAPOLLFD p{s, POLLRDNORM, 0};
    return ::WSAPoll(&p, 1, timeout_ms) > 0;
#else
    pollfd p{s, POLLIN, 0};
    return ::poll(&p, 1, timeout_ms) > 0;
#endif
}

bool accept_should_retry(int e) {
#ifdef _WIN32
    // A client that resets before being accepted is not fatal (on Linux such
    // connections are never reported by accept()).
    return e == WSAECONNRESET;
#else
    return e == EINTR;
#endif
}

struct Iface {
    in_addr addr{};
    in_addr broadcast{};
    bool loopback = false;
    bool can_broadcast = false;
};

// IPv4 addresses of the interfaces that are up.
std::vector<Iface> interfaces() {
    std::vector<Iface> out;
#ifdef _WIN32
    std::vector<unsigned char> buf;
    ULONG size = 16 * 1024, rc;
    do {
        buf.resize(size);
        rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                  nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    } while (rc == ERROR_BUFFER_OVERFLOW);
    if (rc != NO_ERROR) return out;
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        bool loopback = a->IfType == IF_TYPE_SOFTWARE_LOOPBACK;
        bool point_to_point = a->IfType == IF_TYPE_PPP || a->IfType == IF_TYPE_TUNNEL;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
            Iface i;
            i.addr = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr;
            i.loopback = loopback;
            ULONG len = u->OnLinkPrefixLength;
            i.can_broadcast = !loopback && !point_to_point && len < 32;
            uint32_t mask = len == 0 ? 0 : ~uint32_t(0) << (32 - len);
            i.broadcast.s_addr = htonl(ntohl(i.addr.s_addr) | ~mask);
            out.push_back(i);
        }
    }
#else
    ifaddrs* ifs = nullptr;
    if (getifaddrs(&ifs) != 0) return out;
    for (ifaddrs* i = ifs; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP)) continue;
        Iface f;
        f.addr = reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr;
        f.loopback = i->ifa_flags & IFF_LOOPBACK;
        f.can_broadcast = (i->ifa_flags & IFF_BROADCAST) && i->ifa_broadaddr;
        if (f.can_broadcast) f.broadcast = reinterpret_cast<sockaddr_in*>(i->ifa_broadaddr)->sin_addr;
        out.push_back(f);
    }
    freeifaddrs(ifs);
#endif
    return out;
}

std::string host_name() {
    char host[256] = "unknown";
    gethostname(host, static_cast<int>(sizeof host - 1));
    return host;
}

bool stderr_is_tty() {
#ifdef _WIN32
    return _isatty(_fileno(stderr));
#else
    return isatty(STDERR_FILENO);
#endif
}

// A file read or written in one go, closed when it goes out of scope. Errors
// are reported with the file's path.
class File {
public:
    static File open_read(const fs::path& p) {
#ifdef _WIN32
        HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
#else
        int h = ::open(p.c_str(), O_RDONLY);
#endif
        if (h == kNoFile) fail_sys("cannot open '" + utf8(p) + "'", file_error());
        return File(h, p);
    }

    // Creates (or truncates) a file; on Windows it is marked hidden, like a
    // dot-file is on POSIX, and can be deleted while still open.
    static File create(const fs::path& p) {
#ifdef _WIN32
        HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_HIDDEN, nullptr);
#else
        int h = ::open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
#endif
        if (h == kNoFile) fail_sys("cannot create '" + utf8(p) + "'", file_error());
        return File(h, p);
    }

    File(const File&) = delete;
    File& operator=(const File&) = delete;
    File(File&& o) noexcept : h_(std::exchange(o.h_, kNoFile)), path_(std::move(o.path_)) {}
    ~File() {
        if (h_ == kNoFile) return;
#ifdef _WIN32
        CloseHandle(h_);
#else
        ::close(h_);
#endif
    }

    // Returns the number of bytes read, 0 at the end of the file.
    size_t read(void* data, size_t n) {
        for (;;) {
#ifdef _WIN32
            DWORD r = 0;
            if (ReadFile(h_, data, static_cast<DWORD>(std::min<size_t>(n, 1 << 30)), &r, nullptr)) return r;
#else
            ssize_t r = ::read(h_, data, n);
            if (r >= 0) return static_cast<size_t>(r);
            if (errno == EINTR) continue;
#endif
            fail_sys("cannot read '" + utf8(path_) + "'", file_error());
        }
    }

    void write_all(const void* data, size_t n) {
        auto* p = static_cast<const char*>(data);
        while (n > 0) {
#ifdef _WIN32
            DWORD w = 0;
            if (!WriteFile(h_, p, static_cast<DWORD>(std::min<size_t>(n, 1 << 30)), &w, nullptr))
                fail_sys("cannot write '" + utf8(path_) + "'", file_error());
#else
            ssize_t w = ::write(h_, p, n);
            if (w < 0 && errno == EINTR) continue;
            if (w < 0) fail_sys("cannot write '" + utf8(path_) + "'", file_error());
#endif
            p += w;
            n -= static_cast<size_t>(w);
        }
    }

private:
    File(native_file_t h, fs::path p) : h_(h), path_(std::move(p)) {}
    native_file_t h_;
    fs::path path_;
};

// Gives a finished file its final name. The partial file was hidden, the
// renamed one must not be.
void publish_file(const fs::path& from, const fs::path& to) {
    fs::rename(from, to);
#ifdef _WIN32
    DWORD attrs = GetFileAttributesW(to.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) SetFileAttributesW(to.c_str(), attrs & ~DWORD(FILE_ATTRIBUTE_HIDDEN));
#endif
}

// The partial file currently being written, removed if we get interrupted.
fs::path::value_type g_partial[4096];
volatile std::sig_atomic_t g_has_partial = 0;

void set_partial(const fs::path& p) {
    g_has_partial = 0;
    const auto& s = p.native();
    size_t n = std::min(s.size(), std::size(g_partial) - 1);
    std::copy_n(s.data(), n, g_partial);
    g_partial[n] = 0;
    g_has_partial = 1;
}

void remove_partial() {
    if (!g_has_partial) return;
#ifdef _WIN32
    DeleteFileW(g_partial);
#else
    ::unlink(g_partial);
#endif
    g_has_partial = 0;
}

[[noreturn]] void stop_on_interrupt() {
    remove_partial();
    const char msg[] = "\nStopped.\n";
#ifdef _WIN32
    DWORD w;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), msg, sizeof msg - 1, &w, nullptr);
    ExitProcess(130);
#else
    (void)!::write(STDERR_FILENO, msg, sizeof msg - 1);
    ::_exit(130);
#endif
}

#ifdef _WIN32
BOOL WINAPI on_console_event(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT) stop_on_interrupt();
    return FALSE;
}
#else
void on_interrupt(int) { stop_on_interrupt(); }
#endif

// Ctrl-C (and SIGTERM, or closing the console on Windows) removes the partial
// file and exits with status 130.
void install_interrupt_handler() {
#ifdef _WIN32
    SetConsoleCtrlHandler(on_console_event, TRUE);
#else
    struct sigaction sa{};
    sa.sa_handler = on_interrupt;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
#endif
}

// Command line arguments as UTF-8 (on Windows argv is in the ANSI code page).
std::vector<std::string> utf8_args(int argc, char** argv) {
#ifdef _WIN32
    (void)argc;
    (void)argv;
    std::vector<std::string> out;
    int n = 0;
    if (wchar_t** w = CommandLineToArgvW(GetCommandLineW(), &n)) {
        for (int i = 1; i < n; ++i) out.push_back(utf8(w[i]));
        LocalFree(w);
    }
    return out;
#else
    return std::vector<std::string>(argv + 1, argv + argc);
#endif
}

// Lets the console display the UTF-8 text we print.
void setup_console() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
}

// A path component that cannot be created as-is on this system.
bool invalid_on_this_os(const std::string& part) {
#ifdef _WIN32
    if (part.find_first_of("<>:\"|?*") != std::string::npos) return true;
    if (std::any_of(part.begin(), part.end(), [](unsigned char c) { return c < 32; })) return true;
    if (part.back() == '.' || part.back() == ' ') return true;  // Windows would silently strip them
    std::string base = part.substr(0, part.find('.'));
    std::transform(base.begin(), base.end(), base.begin(), [](unsigned char c) { return std::toupper(c); });
    while (!base.empty() && base.back() == ' ') base.pop_back();
    static const char* reserved[] = {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"};
    if (std::any_of(std::begin(reserved), std::end(reserved), [&](const char* r) { return base == r; })) return true;
    if (base.size() == 4 && (base.starts_with("COM") || base.starts_with("LPT")) && base[3] >= '0' && base[3] <= '9')
        return true;
    return false;
#else
    (void)part;
    return false;
#endif
}

// ---------------------------------------------------------------- utilities

std::string human_size(double bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int u = 0;
    while (bytes >= 1024 && u < 4) { bytes /= 1024; ++u; }
    char buf[32];
    std::snprintf(buf, sizeof buf, u == 0 ? "%.0f %s" : "%.1f %s", bytes, units[u]);
    return buf;
}

std::string addr_str(const sockaddr_in& a) {
    char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &a.sin_addr, buf, sizeof buf);
    return buf;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

void write_all(socket_t fd, const void* data, size_t n) {
    auto* p = static_cast<const char*>(data);
    while (n > 0) {
        long long w = sock_send(fd, p, n);
        if (w < 0) {
            int e = sock_error();
            if (is_interrupted(e)) continue;
            fail_sys("connection lost", e);
        }
        p += w;
        n -= static_cast<size_t>(w);
    }
}

// Receives up to `n` bytes (at least one); fails on errors, timeouts and EOF.
size_t recv_some(socket_t fd, void* data, size_t n, const char* eof_msg) {
    for (;;) {
        long long r = sock_recv(fd, data, n);
        if (r < 0) {
            int e = sock_error();
            if (is_interrupted(e)) continue;
            if (is_timeout(e)) fail("connection timed out");
            fail_sys("connection lost", e);
        }
        if (r == 0) fail(eof_msg);
        return static_cast<size_t>(r);
    }
}

void read_all(socket_t fd, void* data, size_t n) {
    auto* p = static_cast<char*>(data);
    while (n > 0) {
        size_t r = recv_some(fd, p, n, "connection closed by peer");
        p += r;
        n -= r;
    }
}

// Big-endian wire encoding.
struct Writer {
    std::vector<char> buf;
    void u8(uint8_t v) { buf.push_back(static_cast<char>(v)); }
    void u16(uint16_t v) { for (int i = 1; i >= 0; --i) u8(static_cast<uint8_t>(v >> (8 * i))); }
    void u32(uint32_t v) { for (int i = 3; i >= 0; --i) u8(static_cast<uint8_t>(v >> (8 * i))); }
    void u64(uint64_t v) { for (int i = 7; i >= 0; --i) u8(static_cast<uint8_t>(v >> (8 * i))); }
    void str(const std::string& s) { u16(static_cast<uint16_t>(s.size())); buf.insert(buf.end(), s.begin(), s.end()); }
    void raw(const void* p, size_t n) { auto* c = static_cast<const char*>(p); buf.insert(buf.end(), c, c + n); }
    void flush(socket_t fd) { write_all(fd, buf.data(), buf.size()); buf.clear(); }
};

uint64_t read_uint(socket_t fd, int bytes) {
    unsigned char b[8];
    read_all(fd, b, static_cast<size_t>(bytes));
    uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) v = (v << 8) | b[i];
    return v;
}

std::string read_str(socket_t fd) {
    auto n = static_cast<size_t>(read_uint(fd, 2));
    std::string s(n, '\0');
    read_all(fd, s.data(), n);
    return s;
}

// ---------------------------------------------------------------- progress

class Progress {
public:
    explicit Progress(uint64_t total) : total_(total), start_(Clock::now()), tty_(stderr_is_tty()) {}

    void add(uint64_t n) {
        done_ += n;
        auto now = Clock::now();
        if (now - last_ > std::chrono::milliseconds(100)) { last_ = now; draw(); }
    }

    void finish() {
        draw();
        if (tty_) std::fputc('\n', stderr);
    }

    double seconds() const { return std::chrono::duration<double>(Clock::now() - start_).count(); }

private:
    void draw() {
        if (!tty_) return;
        double frac = total_ ? static_cast<double>(done_) / static_cast<double>(total_) : 1.0;
        int width = 30, filled = static_cast<int>(frac * width);
        double secs = std::max(seconds(), 1e-3);
        std::string bar(static_cast<size_t>(filled), '#');
        bar.resize(static_cast<size_t>(width), ' ');
        std::fprintf(stderr, "\r  [%s] %3d%%  %s / %s  %s/s   ", bar.c_str(), static_cast<int>(frac * 100),
                     human_size(static_cast<double>(done_)).c_str(), human_size(static_cast<double>(total_)).c_str(),
                     human_size(static_cast<double>(done_) / secs).c_str());
        std::fflush(stderr);
    }

    uint64_t total_, done_ = 0;
    Clock::time_point start_, last_{};
    bool tty_;
};

// ---------------------------------------------------------------- discovery

struct Peer {
    std::string name;
    sockaddr_in addr{};  // TCP endpoint of the receiver
    std::string instance;
    std::string folder;  // name of the folder it saves into; empty for older receivers
};

std::vector<in_addr> broadcast_addresses() {
    std::vector<in_addr> out;
    for (const Iface& i : interfaces())
        if (i.can_broadcast) out.push_back(i.broadcast);
    in_addr a{};
    a.s_addr = htonl(INADDR_BROADCAST);
    out.push_back(a);
    a.s_addr = htonl(INADDR_LOOPBACK);  // receivers on this same machine
    out.push_back(a);
    return out;
}

std::vector<std::string> local_addresses() {
    std::vector<std::string> out;
    for (const Iface& i : interfaces()) {
        if (i.loopback) continue;
        sockaddr_in a{};
        a.sin_addr = i.addr;
        out.push_back(addr_str(a));
    }
    return out;
}

bool name_matches(const std::string& have, const std::string& want) { return lower(have) == lower(want); }

// Queries the network for receivers. If `target` is set only that host is asked
// (unicast), otherwise every broadcast address is. If `want` is set, returns
// shortly after a receiver with that name answers instead of waiting for the
// full timeout.
std::vector<Peer> discover(const std::optional<std::string>& want, const std::optional<in_addr>& target,
                           int timeout_ms) {
    Fd sock(udp_socket());
    if (!sock.valid()) fail_sock("socket");
    set_opt(sock, SOL_SOCKET, SO_BROADCAST, 1);

    std::vector<in_addr> targets = target ? std::vector<in_addr>{*target} : broadcast_addresses();
    auto send_queries = [&] {
        for (const in_addr& t : targets) {
            sockaddr_in to{};
            to.sin_family = AF_INET;
            to.sin_port = htons(kDiscoveryPort);
            to.sin_addr = t;
            udp_sendto(sock, kQueryFolder, std::strlen(kQueryFolder), to);
        }
    };

    std::vector<Peer> peers;
    auto start = Clock::now();
    auto deadline = start + std::chrono::milliseconds(timeout_ms);
    const int resend_at[] = {250, 700};
    size_t next_resend = 0;
    std::vector<char> buf(65536);  // the largest UDP datagram
    send_queries();

    for (;;) {
        auto now = Clock::now();
        if (now >= deadline) break;
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
        if (next_resend < std::size(resend_at) && elapsed >= resend_at[next_resend]) {
            send_queries();
            ++next_resend;
        }
        auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        if (next_resend < std::size(resend_at)) wait = std::min<long long>(wait, resend_at[next_resend] - elapsed);
        if (!wait_readable(sock, static_cast<int>(std::max<long long>(wait, 1)))) continue;

        sockaddr_in from{};
        long long n = udp_recvfrom(sock, buf.data(), buf.size(), from);
        if (n <= 0) continue;
        // "SHARE1 A <tcp-port> <instance> <name>[\n<folder>]"
        std::string msg(buf.data(), static_cast<size_t>(n));
        if (msg.rfind(kAnswerPrefix, 0) != 0) continue;
        std::string rest = msg.substr(std::strlen(kAnswerPrefix));
        size_t s1 = rest.find(' '), s2 = s1 == std::string::npos ? s1 : rest.find(' ', s1 + 1);
        if (s2 == std::string::npos) continue;
        Peer peer;
        int port = std::atoi(rest.substr(0, s1).c_str());
        if (port <= 0 || port > 65535) continue;
        peer.instance = rest.substr(s1 + 1, s2 - s1 - 1);
        peer.name = rest.substr(s2 + 1);
        if (size_t nl = peer.name.find('\n'); nl != std::string::npos) {
            peer.folder = peer.name.substr(nl + 1);
            peer.name.resize(nl);
        }
        peer.addr = from;
        peer.addr.sin_port = htons(static_cast<uint16_t>(port));
        bool dup = std::any_of(peers.begin(), peers.end(), [&](const Peer& p) { return p.instance == peer.instance; });
        if (dup) continue;
        if (want && name_matches(peer.name, *want)) {
            // Give other receivers with the same name a moment to answer, so
            // ambiguity can be detected, then stop.
            deadline = std::min(deadline, Clock::now() + std::chrono::milliseconds(150));
        }
        peers.push_back(std::move(peer));
    }
    return peers;
}

void print_peers(const std::vector<Peer>& peers, FILE* out = stderr) {
    for (const Peer& p : peers) std::fprintf(out, "  %-24s %s\n", p.name.c_str(), addr_str(p.addr).c_str());
}

std::optional<in_addr> resolve_host(const std::string& host) {
    in_addr a{};
    if (inet_pton(AF_INET, host.c_str(), &a) == 1) return a;
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return std::nullopt;
    a = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return a;
}

// ---------------------------------------------------------------- send

struct Entry {
    EntryType type;
    std::string rel;  // path on the wire, '/'-separated
    fs::path src;
    uint64_t size = 0;
};

std::vector<Entry> collect(const std::vector<std::string>& args) {
    std::vector<Entry> entries;
    for (const std::string& arg : args) {
        fs::path p = fs::absolute(from_utf8(arg)).lexically_normal();
        if (p.has_relative_path() && p.filename().empty()) p = p.parent_path();  // trailing slash
        std::error_code ec;
        auto st = fs::status(p, ec);
        if (ec || !fs::exists(st)) fail("cannot access '" + arg + "': no such file or directory");
        std::string base = utf8(p.filename());
        if (base.empty()) fail("cannot send '" + arg + "'");

        if (fs::is_regular_file(st)) {
            entries.push_back({kFile, base, p, static_cast<uint64_t>(fs::file_size(p))});
        } else if (fs::is_directory(st)) {
            entries.push_back({kDir, base, p, 0});
            std::vector<Entry> sub;
            for (auto it = fs::recursive_directory_iterator(p, fs::directory_options::skip_permission_denied);
                 it != fs::recursive_directory_iterator(); ++it) {
                std::string rel = base + "/" + utf8_generic(fs::relative(it->path(), p));
                auto est = it->status(ec);
                if (ec) continue;
                if (fs::is_directory(est)) sub.push_back({kDir, rel, it->path(), 0});
                else if (fs::is_regular_file(est)) sub.push_back({kFile, rel, it->path(), static_cast<uint64_t>(it->file_size())});
            }
            std::sort(sub.begin(), sub.end(), [](const Entry& a, const Entry& b) { return a.rel < b.rel; });
            entries.insert(entries.end(), sub.begin(), sub.end());
        } else {
            fail("cannot send '" + arg + "': not a regular file or directory");
        }
    }
    return entries;
}

Peer find_receiver(const std::string& id) {
    // Accepted forms: "name", "name@host", "host" (IP or hostname).
    std::optional<std::string> want = id;
    std::optional<in_addr> target;
    if (auto at = id.rfind('@'); at != std::string::npos) {
        want = id.substr(0, at);
        target = resolve_host(id.substr(at + 1));
        if (!target) fail("cannot resolve host '" + id.substr(at + 1) + "'");
    }

    std::vector<Peer> peers = discover(want, target, 1500);
    std::vector<Peer> matches;
    for (const Peer& p : peers)
        if (name_matches(p.name, *want)) matches.push_back(p);

    if (matches.empty() && !target) {
        // Maybe the identifier is a host address: ask it directly.
        in_addr a{};
        if (inet_pton(AF_INET, id.c_str(), &a) == 1) {
            auto direct = discover(std::nullopt, a, 1000);
            if (direct.size() == 1) return direct.front();
            if (direct.size() > 1) {
                std::fprintf(stderr, "Several receivers are running on %s, pick one with name@%s:\n", id.c_str(), id.c_str());
                print_peers(direct);
                fail("ambiguous receiver");
            }
        }
    }

    if (matches.size() == 1) return matches.front();
    if (matches.size() > 1) {
        std::fprintf(stderr, "Several receivers are called '%s', pick one with %s@<address>:\n", want->c_str(), want->c_str());
        print_peers(matches);
        fail("ambiguous receiver");
    }
    std::fprintf(stderr, "No receiver called '%s' found.\n", id.c_str());
    if (!peers.empty()) {
        std::fprintf(stderr, "Receivers currently visible:\n");
        print_peers(peers);
    } else {
        std::fprintf(stderr, "No receivers are visible. Is 'share recv' running on the other machine,\n"
                             "on the same network, with UDP port %u open?\n", kDiscoveryPort);
    }
    fail("receiver not found");
}

int cmd_list() {
    std::vector<Peer> peers = discover(std::nullopt, std::nullopt, 1200);
    if (peers.empty()) {
        std::printf("No receivers found. Start one with 'share recv' on another machine.\n");
        return 1;
    }
    std::printf("Receivers on the network:\n");
    std::printf("  %-24s %-15s %s\n", "NAME", "ADDRESS", "FOLDER");
    for (const Peer& p : peers)
        std::printf("  %-24s %-15s %s\n", p.name.c_str(), addr_str(p.addr).c_str(),
                    p.folder.empty() ? "(unknown, older version)" : p.folder.c_str());
    return 0;
}

int cmd_send(const std::string& id, const std::vector<std::string>& paths) {
    std::vector<Entry> entries = collect(paths);
    uint64_t total = 0, files = 0;
    for (const Entry& e : entries)
        if (e.type == kFile) { total += e.size; ++files; }

    Peer peer = find_receiver(id);
    std::printf("Sending %llu file%s (%s) to '%s' at %s\n", static_cast<unsigned long long>(files), files == 1 ? "" : "s",
                human_size(static_cast<double>(total)).c_str(), peer.name.c_str(), addr_str(peer.addr).c_str());

    Fd sock(::socket(AF_INET, SOCK_STREAM, 0));
    if (!sock.valid()) fail_sock("socket");
    if (::connect(sock, reinterpret_cast<sockaddr*>(&peer.addr), sizeof peer.addr) < 0)
        fail_sock("cannot connect to " + addr_str(peer.addr) + ":" + std::to_string(ntohs(peer.addr.sin_port)));

    Writer w;
    w.raw(kMagic, sizeof kMagic);
    w.str(peer.name);
    w.str(host_name());
    w.u32(static_cast<uint32_t>(entries.size()));
    w.u64(total);
    w.flush(sock);

    uint8_t reply = static_cast<uint8_t>(read_uint(sock, 1));
    if (reply != kAccept) fail("the receiver refused the transfer");

    Progress progress(total);
    std::vector<char> buf(kChunk);
    for (const Entry& e : entries) {
        w.u8(e.type);
        w.str(e.rel);
        if (e.type == kDir) { w.flush(sock); continue; }
        w.u64(e.size);
        w.flush(sock);

        File in = File::open_read(e.src);
        uint64_t left = e.size;
        while (left > 0) {
            size_t r = in.read(buf.data(), static_cast<size_t>(std::min<uint64_t>(left, buf.size())));
            if (r == 0) fail("'" + utf8(e.src) + "' shrank while being sent");
            write_all(sock, buf.data(), r);
            left -= r;
            progress.add(r);
        }
    }
    ::shutdown(sock, kShutWrite);
    uint8_t done = static_cast<uint8_t>(read_uint(sock, 1));
    progress.finish();
    if (done != kDone) fail("the receiver reported an error");
    std::printf("Done: %s in %.1fs\n", human_size(static_cast<double>(total)).c_str(), progress.seconds());
    return 0;
}

// ---------------------------------------------------------------- receive

// "photo.jpg" -> "photo (1).jpg" if taken, and so on.
fs::path unique_path(const fs::path& p) {
    std::error_code ec;
    if (!fs::exists(fs::symlink_status(p, ec))) return p;
    std::string stem = utf8(p.stem()), ext = utf8(p.extension());
    if (stem.empty() || stem.front() == '.') { stem = utf8(p.filename()); ext.clear(); }
    for (int i = 1;; ++i) {
        fs::path c = p.parent_path() / from_utf8(stem + " (" + std::to_string(i) + ")" + ext);
        if (!fs::exists(fs::symlink_status(c, ec))) return c;
    }
}

// Validates a wire path and returns its components; rejects anything that
// could escape the destination directory, or that this OS cannot store.
std::vector<std::string> split_safe(const std::string& rel) {
    std::vector<std::string> parts;
    if (rel.empty() || rel.front() == '/') fail("invalid path from sender: '" + rel + "'");
    size_t start = 0;
    for (;;) {
        size_t slash = rel.find('/', start);
        std::string part = rel.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (part.empty() || part == "." || part == ".." || part.find('\0') != std::string::npos ||
            part.find('\\') != std::string::npos || invalid_on_this_os(part))
            fail("invalid path from sender: '" + rel + "'");
        parts.push_back(part);
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return parts;
}

void receive_one(socket_t sock, const sockaddr_in& from, const std::string& name, const fs::path& dest) {
    set_recv_timeout(sock, 60);

    char magic[4];
    read_all(sock, magic, sizeof magic);
    if (std::memcmp(magic, kMagic, sizeof magic) != 0) fail("unknown client from " + addr_str(from));
    std::string target = read_str(sock);
    std::string sender = read_str(sock);
    auto count = static_cast<uint32_t>(read_uint(sock, 4));
    uint64_t total = read_uint(sock, 8);
    if (!name_matches(target, name)) {
        uint8_t no = kReject;
        write_all(sock, &no, 1);
        fail("rejected transfer meant for '" + target + "'");
    }
    uint8_t yes = kAccept;
    write_all(sock, &yes, 1);

    std::printf("\nIncoming from %s (%s): %s\n", sender.c_str(), addr_str(from).c_str(),
                human_size(static_cast<double>(total)).c_str());
    std::fflush(stdout);

    std::map<std::string, std::string> top_rename;  // top-level name -> name actually used
    std::vector<fs::path> saved;
    Progress progress(total);
    std::vector<char> buf(kChunk);

    for (uint32_t i = 0; i < count; ++i) {
        auto type = static_cast<uint8_t>(read_uint(sock, 1));
        std::vector<std::string> parts = split_safe(read_str(sock));
        if (type != kFile && type != kDir) fail("protocol error");

        auto it = top_rename.find(parts[0]);
        if (it == top_rename.end()) {
            std::string used = utf8(unique_path(dest / from_utf8(parts[0])).filename());
            it = top_rename.emplace(parts[0], used).first;
            saved.push_back(dest / from_utf8(used));
        }
        fs::path out = dest / from_utf8(it->second);
        for (size_t k = 1; k < parts.size(); ++k) out /= from_utf8(parts[k]);

        if (type == kDir) {
            fs::create_directories(out);
            continue;
        }

        uint64_t size = read_uint(sock, 8);
        fs::create_directories(out.parent_path());
        fs::path part = out.parent_path() / from_utf8("." + utf8(out.filename()) + ".share-part");
        set_partial(part);
        {
            File f = File::create(part);
            uint64_t left = size;
            while (left > 0) {
                size_t r = recv_some(sock, buf.data(), static_cast<size_t>(std::min<uint64_t>(left, buf.size())),
                                     "sender disconnected");
                f.write_all(buf.data(), r);
                left -= r;
                progress.add(r);
            }
        }
        publish_file(part, unique_path(out));
        g_has_partial = 0;
    }
    uint8_t done = kDone;
    write_all(sock, &done, 1);
    progress.finish();

    for (const fs::path& p : saved) std::printf("  saved %s\n", utf8(fs::relative(p, dest)).c_str());
    std::printf("Received %s in %.1fs\n", human_size(static_cast<double>(total)).c_str(), progress.seconds());
    std::fflush(stdout);
}

void responder(socket_t udp, std::string name, std::string folder, uint16_t tcp_port, std::string instance) {
    std::string answer = std::string(kAnswerPrefix) + std::to_string(tcp_port) + " " + instance + " " + name;
    std::string answer_folder = answer + "\n" + folder;  // only for kQueryFolder: older senders can't parse it
    char buf[512];
    for (;;) {
        sockaddr_in from{};
        long long n = udp_recvfrom(udp, buf, sizeof buf, from);
        if (n < 0 && is_interrupted(sock_error())) continue;
        if (n < 0) return;
        if (static_cast<size_t>(n) < std::strlen(kQuery) || std::memcmp(buf, kQuery, std::strlen(kQuery)) != 0) continue;
        bool wants_folder = static_cast<size_t>(n) >= std::strlen(kQueryFolder) &&
                            std::memcmp(buf, kQueryFolder, std::strlen(kQueryFolder)) == 0;
        const std::string& reply = wants_folder ? answer_folder : answer;
        udp_sendto(udp, reply.data(), reply.size(), from);
    }
}

int cmd_recv(std::optional<std::string> name_arg) {
    fs::path dest = fs::current_path();
    std::string name = name_arg ? *name_arg : utf8(dest.filename());
    if (name.empty()) name = "share";
    if (name.size() > kMaxName) fail("name too long");
    if (name.find('@') != std::string::npos || name.find('\n') != std::string::npos)
        fail("the name cannot contain '@' or newlines");

    // TCP: the fixed port if free (easy to allow through a firewall), else any.
    Fd tcp(::socket(AF_INET, SOCK_STREAM, 0));
    if (!tcp.valid()) fail_sock("socket");
    allow_tcp_port_reuse(tcp);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(kPreferredTcpPort);
    if (::bind(tcp, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
        addr.sin_port = 0;
        if (::bind(tcp, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) fail_sock("bind");
    }
    if (::listen(tcp, 8) < 0) fail_sock("listen");
    socklen_t al = sizeof addr;
    getsockname(tcp, reinterpret_cast<sockaddr*>(&addr), &al);
    uint16_t tcp_port = ntohs(addr.sin_port);

    // Warn if the name is already taken by another receiver.
    for (const Peer& p : discover(name, std::nullopt, 400))
        if (name_matches(p.name, name))
            std::fprintf(stderr, "warning: another receiver called '%s' is running at %s; senders will need %s@<address>\n",
                         p.name.c_str(), addr_str(p.addr).c_str(), name.c_str());

    // UDP: shared discovery port, several receivers may run on one machine.
    Fd udp(udp_socket());
    if (!udp.valid()) fail_sock("socket");
    allow_shared_udp_port(udp);
    sockaddr_in uaddr{};
    uaddr.sin_family = AF_INET;
    uaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    uaddr.sin_port = htons(kDiscoveryPort);
    if (::bind(udp, reinterpret_cast<sockaddr*>(&uaddr), sizeof uaddr) < 0)
        fail_sock("cannot listen on UDP port " + std::to_string(kDiscoveryPort));

    std::random_device rd;
    char instance[17];
    std::snprintf(instance, sizeof instance, "%08x%08x", rd(), rd());
    // Only the folder's own name is advertised, not its full path ("/" or "C:\\" as they are).
    std::string folder = dest.has_filename() ? utf8(dest.filename()) : utf8(dest);
    std::thread(responder, socket_t(udp), name, folder, tcp_port, std::string(instance)).detach();

    install_interrupt_handler();

    std::printf("Receiving as '%s' into %s\n", name.c_str(), utf8(dest).c_str());
    std::string ips;
    for (const std::string& ip : local_addresses()) ips += (ips.empty() ? "" : ", ") + ip;
    if (!ips.empty()) std::printf("Addresses: %s (tcp %u)\n", ips.c_str(), tcp_port);
    std::printf("On the other machine run:  share send %s <file>...\n", name.c_str());
    std::printf("Press Ctrl-C to stop.\n");
    std::fflush(stdout);

    for (;;) {
        sockaddr_in from{};
        socklen_t fl = sizeof from;
        Fd conn(::accept(tcp, reinterpret_cast<sockaddr*>(&from), &fl));
        if (!conn.valid()) {
            int e = sock_error();
            if (accept_should_retry(e)) continue;
            fail_sys("accept", e);
        }
        try {
            receive_one(conn, from, name, dest);
        } catch (const std::exception& e) {
            remove_partial();
            std::fprintf(stderr, "\ntransfer failed: %s\n", e.what());
        }
    }
}

// ---------------------------------------------------------------- main

void usage(FILE* out) {
    std::fprintf(out,
                 "share %s - send files to another machine on the local network\n"
                 "\n"
                 "Usage:\n"
                 "  share recv [name]             wait for files, saved into the current directory.\n"
                 "                                 <name> defaults to the directory's name.\n"
                 "  share send <name> <path>...   send files or directories to the receiver <name>.\n"
                 "  share list                    list the receivers on the network and their folders.\n"
                 "  share send                    same as 'share list'.\n"
                 "\n"
                 "'s', 'r' and 'l' are shortcuts for 'send', 'recv' and 'list'.\n"
                 "<name> can also be name@host, or the receiver's IP address.\n"
                 "Ports: UDP %u (discovery), TCP %u (transfer).\n",
                 kVersion, kDiscoveryPort, kPreferredTcpPort);
}

}  // namespace

int main(int argc, char** argv) {
    setup_console();
    std::vector<std::string> args = utf8_args(argc, argv);
    try {
        NetInit net;
        if (args.empty() || args[0] == "-h" || args[0] == "--help" || args[0] == "help") {
            usage(args.empty() ? stderr : stdout);
            return args.empty() ? 2 : 0;
        }
        if (args[0] == "-V" || args[0] == "--version") {
            std::printf("share %s\n", kVersion);
            return 0;
        }
        if (args[0] == "recv" || args[0] == "receive" || args[0] == "r") {
            if (args.size() > 2) { usage(stderr); return 2; }
            return cmd_recv(args.size() == 2 ? std::optional<std::string>(args[1]) : std::nullopt);
        }
        if (args[0] == "list" || args[0] == "ls" || args[0] == "l") {
            if (args.size() > 1) { usage(stderr); return 2; }
            return cmd_list();
        }
        if (args[0] == "send" || args[0] == "s") {
            if (args.size() == 1) return cmd_list();
            if (args.size() == 2) {
                std::fprintf(stderr, "Missing the files to send: share send %s <path>...\n", args[1].c_str());
                return 2;
            }
            return cmd_send(args[1], std::vector<std::string>(args.begin() + 2, args.end()));
        }
        std::fprintf(stderr, "Unknown command '%s'.\n\n", args[0].c_str());
        usage(stderr);
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "share: %s\n", e.what());
        return 1;
    }
}
