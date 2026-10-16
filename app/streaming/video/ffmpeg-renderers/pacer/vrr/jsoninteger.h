#pragma once
#include <QJsonValue>
#include <cmath>
#include <limits>

namespace Vrr13 {
// Qt 5 stores JSON numbers as doubles and has no QJsonValue::toInteger().
// Check the exclusive upper bound before casting: double(INT64_MAX) is 2^63.
inline qint64 jsonInteger(const QJsonValue& value, qint64 fallback = 0)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return value.toInteger(fallback);
#else
    if (!value.isDouble()) return fallback;
    const double number = value.toDouble();
    const double minimum = double(std::numeric_limits<qint64>::min());
    if (!std::isfinite(number) || number < minimum || number >= -minimum ||
        std::trunc(number) != number) return fallback;
    return qint64(number);
#endif
}
}
