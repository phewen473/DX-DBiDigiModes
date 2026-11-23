#ifndef QT_STATIC_REGEX_HPP__
#define QT_STATIC_REGEX_HPP__

// DX-DBi v1R0: compile a fixed regular expression once per call site.
//
// A temporary QRegularExpression {"..."} is compiled every time it is used;
// in the per-decode paths that meant several compilations for every decoded
// line.  STATIC_REGEX ("...") yields a reference to a function-local static
// that is compiled on first use only.  Use it only with constant patterns.

#include <QRegularExpression>

#define STATIC_REGEX(...) \
  ([] () -> QRegularExpression const& { static QRegularExpression const re__ {__VA_ARGS__}; return re__; } ())

#endif
