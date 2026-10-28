#include "SerialPortName.h"
#include "SerialControllerTransport.h"
#include <iostream>
#include <stdexcept>

namespace
{
void require (bool value, const char* description)
{
    if (! value) throw std::runtime_error (description);
}
}

int main()
{
    using namespace mixsense;
    try
    {
        require (normalizeSerialPortName ("com12") == "COM12", "lowercase COM convenience retained");
        require (isWindowsSerialPortName ("COM12") && ! isWindowsSerialPortName ("COM12/other"), "COM syntax");
        const std::string path = "/dev/cu.usbmodemMiXeD_123";
        require (isMacSerialPortName (path) && normalizeSerialPortName (path) == path, "macOS device case preserved");
        require (! isMacSerialPortName ("/dev/cu.") && ! isMacSerialPortName ("/dev/tty.usbmodem123"), "callout device required");
        require (! isMacSerialPortName ("/dev/cu.test/../../tmp/file"), "nested path rejected");
        require (! isMacSerialPortName (std::string ("/dev/cu.test\0suffix", 19)), "embedded NUL rejected");
        // Never open a real port in host-only tests. Exercise the selected native
        // factory's closed/error path on whichever platform builds this target.
        auto native = makeNativeSerialByteStream();
        std::uint8_t byte = 0;
        std::size_t count = 99;
        require (! native->read (&byte, 1, count) && count == 0, "read on closed native backend");
        count = 99;
        require (! native->write (&byte, 1, count) && count == 0, "write on closed native backend");
        require (! native->open ("not-a-serial-device"), "invalid native path rejected");
        native->close(); native->close();
        std::cout << "Port naming and native backend closed/error paths passed\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
