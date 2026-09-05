#pragma once
#include "block.h"
#include <string>
namespace clickhouse {
class ClientOptions {
public:
    ClientOptions& SetHost(const std::string&){return *this;}
    ClientOptions& SetPort(unsigned int){return *this;}
    ClientOptions& SetUser(const std::string&){return *this;}
    ClientOptions& SetPassword(const std::string&){return *this;}
    ClientOptions& SetDefaultDatabase(const std::string&){return *this;}
};
class Client {
public:
    explicit Client(const ClientOptions&){}
    void Insert(const std::string&, const Block&){}
};
}
