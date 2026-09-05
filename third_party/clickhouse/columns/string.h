#pragma once
#include "column.h"
#include <string>
namespace clickhouse { class ColumnString : public Column { public: template<typename U> void Append(U){} }; }
