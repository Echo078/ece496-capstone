#include "MacSerialByteStream.h"
#include "SerialPortName.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace mixsense::detail
{
bool MacSerialByteStream::open (const std::string& port)
{
    close();
    if (! isMacSerialPortName (port)) return false;
    descriptor = ::open (port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    return configure();
}

bool MacSerialByteStream::configure()
{
    if (descriptor < 0) return false;
    struct stat info {};
    if (fstat (descriptor, &info) != 0 || ! S_ISCHR (info.st_mode)
        || ioctl (descriptor, TIOCEXCL) != 0)
    { close(); return false; }
    exclusive = true;
    if (tcgetattr (descriptor, &originalSettings) != 0) { close(); return false; }
    settingsSaved = true;
    auto settings = originalSettings;
    cfmakeraw (&settings);
    settings.c_cflag &= ~(CIGNORE | CSIZE | PARENB | PARODD | CSTOPB | CRTSCTS | CDTR_IFLOW | CDSR_OFLOW | CCAR_OFLOW);
    settings.c_cflag |= CS8 | CLOCAL | CREAD;
    settings.c_iflag &= ~(IXON | IXOFF | IXANY);
    // Keep O_NONBLOCK throughout the session. VMIN=1 lets a zero-byte read
    // represent hangup rather than a configured VMIN=0 idle read on Darwin TTYs.
    settings.c_cc[VMIN] = 1;
    settings.c_cc[VTIME] = 0;
    if (cfsetispeed (&settings, B115200) != 0 || cfsetospeed (&settings, B115200) != 0
        || tcsetattr (descriptor, TCSANOW, &settings) != 0 || tcflush (descriptor, TCIOFLUSH) != 0)
    { close(); return false; }
    return true;
}

void MacSerialByteStream::close() noexcept
{
    if (descriptor < 0) return;
    // Never tcdrain(): queued output must not hold shutdown waiting for a device.
    tcflush (descriptor, TCIOFLUSH);
    if (settingsSaved) tcsetattr (descriptor, TCSANOW, &originalSettings);
    if (exclusive) ioctl (descriptor, TIOCNXCL);
    ::close (descriptor);
    descriptor = -1;
    settingsSaved = false;
    exclusive = false;
}

bool MacSerialByteStream::read (std::uint8_t* bytes, std::size_t capacity, std::size_t& received)
{
    received = 0;
    if (descriptor < 0) return false;
    if (capacity == 0) return true;
    const auto result = ::read (descriptor, bytes, capacity);
    if (result > 0) { received = static_cast<std::size_t> (result); return true; }
    if (result == 0) return false; // EOF/hangup, not an empty application frame.
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}

bool MacSerialByteStream::write (const std::uint8_t* bytes, std::size_t size, std::size_t& written)
{
    written = 0;
    if (descriptor < 0 || size == 0) return false;
    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::milliseconds (50);
    for (;;)
    {
        const auto result = ::write (descriptor, bytes, size);
        if (result > 0) { written = static_cast<std::size_t> (result); return true; }
        if (result == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) return false;
        const auto remaining = deadline - Clock::now();
        if (remaining <= Clock::duration::zero()) return false;
        // Worker-only bounded backoff. Keep reads nonblocking and avoid relying
        // on poll(2) character-device support across Darwin versions.
        std::this_thread::sleep_for (std::min (remaining, Clock::duration (std::chrono::milliseconds (2))));
    }
}
}

namespace mixsense
{
std::unique_ptr<ISerialByteStream> makeNativeSerialByteStream()
{
    return std::make_unique<detail::MacSerialByteStream>();
}
}
