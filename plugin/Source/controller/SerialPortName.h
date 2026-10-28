#pragma once

#include <string>

namespace mixsense
{
inline bool isWindowsSerialPortName (const std::string& name) noexcept
{
    if (name.size() < 4 || (name[0] != 'C' && name[0] != 'c')
        || (name[1] != 'O' && name[1] != 'o') || (name[2] != 'M' && name[2] != 'm')) return false;
    for (std::size_t i = 3; i < name.size(); ++i)
        if (name[i] < '0' || name[i] > '9') return false;
    return true;
}

inline bool isMacSerialPortName (const std::string& name) noexcept
{
    if (name.compare (0, 8, "/dev/cu.") != 0 || name.size() == 8) return false;
    // A single device-file component only: no nested paths or embedded NULs.
    for (std::size_t i = 8; i < name.size(); ++i)
        if (name[i] == '/' || name[i] == '\0') return false;
    return true;
}

inline std::string normalizeSerialPortName (std::string name)
{
    if (isWindowsSerialPortName (name)) name.replace (0, 3, "COM");
    return name; // POSIX paths (and mock names) retain their exact case.
}
}
