#pragma once
// PORT: This game is built in its original narrow-character configuration.
#include <cctype>
#define _T(value) value
#define _TEXT(value) value
#define _tcsinc(value) ((value) + 1)
#define _istdigit(value) std::isdigit(static_cast<unsigned char>(value))
