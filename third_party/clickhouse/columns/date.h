#pragma once
#include "column.h"
#include <cstdint>
namespace clickhouse {
class ColumnDate : public Column { public: template<typename U> void Append(U){} };
class ColumnDateTime : public Column { public: template<typename U> void Append(U){} };
}
