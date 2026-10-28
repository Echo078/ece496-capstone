#pragma once

// Internal macOS backend; neither termios nor descriptors enter the middleware API.
#include "SerialControllerTransport.h"
#include <termios.h>

namespace mixsense::detail
{
class MacSerialByteStream final : public ISerialByteStream
{
public:
    MacSerialByteStream() = default;
    MacSerialByteStream (const MacSerialByteStream&) = delete;
    MacSerialByteStream& operator= (const MacSerialByteStream&) = delete;
    ~MacSerialByteStream() override { close(); }
    bool open (const std::string& port) override;
    void close() noexcept override;
    bool read (std::uint8_t*, std::size_t capacity, std::size_t& received) override;
    bool write (const std::uint8_t*, std::size_t size, std::size_t& written) override;
private:
    bool configure();
    int descriptor = -1;
    termios originalSettings {};
    bool settingsSaved = false;
    bool exclusive = false;
    // Tests adopt a PTY slave without weakening production /dev/cu.* validation.
    friend struct MacSerialByteStreamTestAccess;
};
}
