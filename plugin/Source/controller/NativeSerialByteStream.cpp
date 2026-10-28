#include "SerialControllerTransport.h"

#if defined(_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif

namespace mixsense
{
namespace
{
class NativeSerialByteStream final : public ISerialByteStream
{
public:
    ~NativeSerialByteStream() override { close(); }
    bool open (const std::string& port) override
    {
        close();
       #if defined(_WIN32)
        // Accept only a selected COM name; never turn this API into arbitrary file I/O.
        if (port.size() < 4 || port.substr (0, 3) != "COM") return false;
        for (std::size_t i = 3; i < port.size(); ++i)
            if (port[i] < '0' || port[i] > '9') return false;
        const std::string path = "\\\\.\\" + port;
        handle = CreateFileA (path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return false;
        DCB settings {};
        settings.DCBlength = sizeof (settings);
        if (! GetCommState (handle, &settings)) { close(); return false; }
        settings.BaudRate = CBR_115200; settings.ByteSize = 8;
        settings.Parity = NOPARITY; settings.StopBits = ONESTOPBIT;
        settings.fBinary = TRUE; settings.fParity = FALSE;
        settings.fOutxCtsFlow = FALSE; settings.fOutxDsrFlow = FALSE;
        settings.fDtrControl = DTR_CONTROL_ENABLE; settings.fRtsControl = RTS_CONTROL_DISABLE;
        settings.fDsrSensitivity = FALSE; settings.fOutX = FALSE; settings.fInX = FALSE;
        settings.fErrorChar = FALSE; settings.fNull = FALSE; settings.fAbortOnError = FALSE;
        settings.XonChar = 0x11; settings.XoffChar = 0x13;
        COMMTIMEOUTS timeouts {};
        timeouts.ReadIntervalTimeout = MAXDWORD; // immediate reads, including zero bytes
        timeouts.WriteTotalTimeoutConstant = 50; // worker-only finite write timeout
        if (! SetCommState (handle, &settings) || ! SetCommTimeouts (handle, &timeouts)
            || ! PurgeComm (handle, PURGE_RXCLEAR | PURGE_TXCLEAR))
        { close(); return false; }
        return true;
       #else
        (void) port;
        return false;
       #endif
    }
    void close() noexcept override
    {
       #if defined(_WIN32)
        if (handle != INVALID_HANDLE_VALUE) { CloseHandle (handle); handle = INVALID_HANDLE_VALUE; }
       #endif
    }
    bool read (std::uint8_t* bytes, std::size_t capacity, std::size_t& count) override
    {
        count = 0;
       #if defined(_WIN32)
        DWORD errors = 0; COMSTAT status {};
        if (handle == INVALID_HANDLE_VALUE || ! ClearCommError (handle, &errors, &status) || errors != 0) return false;
        DWORD received = 0;
        if (! ReadFile (handle, bytes, static_cast<DWORD> (capacity), &received, nullptr)) return false;
        count = received;
        return true;
       #else
        (void) bytes; (void) capacity; return false;
       #endif
    }
    bool write (const std::uint8_t* bytes, std::size_t size, std::size_t& count) override
    {
        count = 0;
       #if defined(_WIN32)
        DWORD written = 0;
        if (handle == INVALID_HANDLE_VALUE || ! WriteFile (handle, bytes, static_cast<DWORD> (size), &written, nullptr)) return false;
        count = written;
        return written != 0;
       #else
        (void) bytes; (void) size; return false;
       #endif
    }
private:
   #if defined(_WIN32)
    HANDLE handle = INVALID_HANDLE_VALUE;
   #endif
};
}
std::unique_ptr<ISerialByteStream> makeNativeSerialByteStream() { return std::make_unique<NativeSerialByteStream>(); }
}
