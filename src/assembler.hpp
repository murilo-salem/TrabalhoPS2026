#pragma once

#include <cstdint>
#include <map>
#include <string>

struct AssemblyResult {
    std::string object;
    std::string listing;
    std::map<std::string, uint32_t> symbols;
};

// Montador SIC/XE de duas passagens. A primeira atribui endereços e constrói
// SYMTAB; a segunda escolhe os modos de endereçamento e emite H/T/M/E.
class Assembler {
public:
    AssemblyResult assemble(const std::string& source) const;
};
