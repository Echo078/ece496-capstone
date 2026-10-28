#include "SerialControllerTransport.h"

namespace mixsense
{
namespace
{
class UnsupportedSerialByteStream final : public ISerialByteStream
{
public:
    bool open (const std::string&) override { return false; }
    void close() noexcept override {}
    bool read (std::uint8_t*, std::size_t, std::size_t& count) override { count = 0; return false; }
    bool write (const std::uint8_t*, std::size_t, std::size_t& count) override { count = 0; return false; }
};
}
std::unique_ptr<ISerialByteStream> makeNativeSerialByteStream() { return std::make_unique<UnsupportedSerialByteStream>(); }
}
