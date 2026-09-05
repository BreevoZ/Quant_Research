#pragma once
#include "columns/column.h"
#include <string>
#include <memory>
namespace clickhouse {
class Block { public: Block(){} void AppendColumn(const std::string&, const std::shared_ptr<Column>&){} };
}
