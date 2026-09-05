#pragma once
#include "column.h"
#include <cstdint>
namespace clickhouse {
template<typename T> class ColumnVectorT : public Column { public: template<typename U> void Append(U){} };
using ColumnFloat64 = ColumnVectorT<double>;
using ColumnInt8    = ColumnVectorT<int8_t>;
using ColumnInt16   = ColumnVectorT<int16_t>;
using ColumnInt32   = ColumnVectorT<int32_t>;
using ColumnInt64   = ColumnVectorT<int64_t>;
using ColumnUInt8   = ColumnVectorT<uint8_t>;
using ColumnUInt16  = ColumnVectorT<uint16_t>;
using ColumnUInt32  = ColumnVectorT<uint32_t>;
using ColumnUInt64  = ColumnVectorT<uint64_t>;
}
