#include "MacSerialByteStream.h"
#include <array>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <sys/ioctl.h>
#include <unistd.h>
#include <util.h>
#include <vector>

namespace mixsense::detail
{
struct MacSerialByteStreamTestAccess
{
    static bool adopt (MacSerialByteStream& stream, int descriptor)
    {
        stream.close();
        stream.descriptor = descriptor;
        return stream.configure();
    }
};
}

namespace
{
using namespace mixsense::detail;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
void require (bool value, const char* description)
{
    if (! value) throw std::runtime_error (description);
}
struct Pty
{
    Pty() { require (openpty (&master, &slave, name.data(), nullptr, nullptr) == 0, "create macOS PTY"); }
    ~Pty() { if (master >= 0) ::close (master); if (slave >= 0) ::close (slave); }
    int master = -1, slave = -1;
    std::array<char, 1024> name {};
};

void configurePty (Pty& pty, MacSerialByteStream& stream)
{
    // PTYs are /dev/ttys*, not /dev/cu.*. Adopt a test-only owned descriptor;
    // production factory/path validation is unchanged and tested separately.
    const int owned = pty.slave; pty.slave = -1;
    require (fcntl (owned, F_SETFL, O_NONBLOCK) == 0, "nonblocking PTY slave");
    require (MacSerialByteStreamTestAccess::adopt (stream, owned), "configure actual Darwin termios backend");
}

void configurationTest()
{
    Pty pty;
    const int observation = pty.slave;
    MacSerialByteStream stream;
    configurePty (pty, stream);
    termios current {};
    require (tcgetattr (observation, &current) == 0, "read configured settings");
    require ((current.c_lflag & (ICANON | ECHO | ISIG)) == 0 && (current.c_oflag & OPOST) == 0, "raw binary mode");
    require ((current.c_cflag & CSIZE) == CS8 && (current.c_cflag & (PARENB | CSTOPB | CRTSCTS)) == 0, "8N1 no hardware flow control");
    require (cfgetispeed (&current) == B115200 && cfgetospeed (&current) == B115200, "serial line coding");
    // Successful configuration includes a successful TIOCEXCL syscall, but
    // Darwin PTYs do not reliably reject subsequent non-root opens. Competing
    // instance exclusion must be verified on a physical /dev/cu.* CDC device.
}

void binaryIoTest()
{
    Pty pty;
    MacSerialByteStream stream;
    configurePty (pty, stream);
    std::array<std::uint8_t, 8> received {};
    std::size_t count = 99;
    const auto start = Clock::now();
    require (stream.read (received.data(), received.size(), count) && count == 0, "idle nonblocking read");
    require (Clock::now() - start < 100ms, "idle read returns promptly");
    const std::array<std::uint8_t, 4> bytes { 0x00, 0x11, 0x13, 0xff };
    require (::write (pty.master, bytes.data(), bytes.size()) == static_cast<ssize_t> (bytes.size()), "write device-side raw bytes");
    const auto readDeadline = Clock::now() + 1s;
    std::size_t total = 0;
    while (total < bytes.size() && Clock::now() < readDeadline)
    {
        require (stream.read (received.data() + total, 1, count), "partial backend reads");
        total += count;
    }
    require (total == bytes.size() && std::memcmp (received.data(), bytes.data(), bytes.size()) == 0, "binary bytes survive termios");
    require (stream.write (bytes.data(), bytes.size(), count) && count > 0 && count <= bytes.size(), "backend output accepts bytes");
}

void cleanupTest()
{
    Pty pty;
    const int observation = dup (pty.slave);
    require (observation >= 0, "duplicate observation descriptor");
    struct Observation
    {
        int descriptor;
        ~Observation() { ::close (descriptor); }
    } observed { observation };
    termios original {}, current {};
    require (tcgetattr (observation, &original) == 0, "read initial settings");
    MacSerialByteStream stream;
    configurePty (pty, stream);
    stream.close(); stream.close();
    require (tcgetattr (observation, &current) == 0, "read restored settings");
    // Darwin sets PENDIN when TCSANOW restores canonical mode. This is
    // kernel-maintained input state, so compare configuration without that bit.
    require ((current.c_lflag & ~PENDIN) == (original.c_lflag & ~PENDIN), "close restores local flags");
    require (current.c_iflag == original.c_iflag && current.c_oflag == original.c_oflag
             && current.c_cflag == original.c_cflag, "close restores input, output and control flags");
    require (cfgetispeed (&current) == cfgetispeed (&original)
             && cfgetospeed (&current) == cfgetospeed (&original), "close restores serial speeds");
    require (std::memcmp (current.c_cc, original.c_cc, sizeof (original.c_cc)) == 0,
             "close restores control characters");
    const int reopened = ::open (pty.name.data(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    require (reopened >= 0, "PTY can be reopened after close");
    ::close (reopened);
}

void backpressureAndHangupTest()
{
    Pty pty;
    MacSerialByteStream stream;
    configurePty (pty, stream);
    const std::vector<std::uint8_t> bytes (1024 * 1024, 0x7f);
    bool timedOut = false;
    const auto deadline = Clock::now() + 2s;
    // Do not drain the master: a bounded kernel queue forces partial writes and
    // eventual EAGAIN. Each backend call must stop retrying within its deadline.
    while (Clock::now() < deadline && ! timedOut)
    {
        std::size_t count = 0;
        const auto start = Clock::now();
        timedOut = ! stream.write (bytes.data(), bytes.size(), count);
        require (Clock::now() - start < 250ms, "write backpressure has bounded wait");
        require (timedOut ? count == 0 : count > 0 && count <= bytes.size(), "partial write result contract");
    }
    require (timedOut, "PTY backpressure reaches timeout");
    ::close (pty.master); pty.master = -1;
    std::uint8_t byte = 0;
    std::size_t count = 0;
    require (! stream.read (&byte, 1, count) && count == 0, "PTY hangup reports failure");
    const auto start = Clock::now();
    stream.close();
    require (Clock::now() - start < 250ms, "close never waits to drain unconsumed output");
}
}

int main (int argc, char* argv[])
{
    struct TestCase
    {
        const char* name;
        void (*run)();
    };
    const TestCase cases[] {
        { "configuration", configurationTest },
        { "binary_io", binaryIoTest },
        { "cleanup", cleanupTest },
        { "backpressure_hangup", backpressureAndHangupTest }
    };
    bool selected = false;
    bool passed = true;
    for (const auto& test : cases)
    {
        if (argc != 1 && (argc != 2 || std::strcmp (argv[1], test.name) != 0)) continue;
        selected = true;
        try
        {
            test.run();
            std::cout << test.name << ": passed\n";
        }
        catch (const std::exception& error)
        {
            std::cerr << test.name << ": " << error.what() << '\n';
            passed = false;
        }
    }
    if (! selected) std::cerr << "Expected no arguments or one case: configuration, binary_io, cleanup, backpressure_hangup\n";
    return selected && passed ? 0 : 1;
}
