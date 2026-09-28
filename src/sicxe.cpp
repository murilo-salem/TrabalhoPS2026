#include "sicxe.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <iomanip>
#include <limits>
#include <sstream>

namespace {
uint32_t hex(const std::string& value) {
    if (value.empty()) throw std::runtime_error("Campo hexadecimal vazio");
    uint64_t result = 0;
    for (char ch : value) {
        if (!std::isxdigit(static_cast<unsigned char>(ch))) throw std::runtime_error("Hexadecimal inválido: " + value);
        result = result * 16 + static_cast<uint32_t>(std::isdigit(static_cast<unsigned char>(ch)) ? ch - '0' : std::toupper(static_cast<unsigned char>(ch)) - 'A' + 10);
        if (result > 0xffffffffu) throw std::runtime_error("Número hexadecimal grande demais");
    }
    return static_cast<uint32_t>(result);
}

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

struct Record { char type; std::vector<std::string> fields; };
Record parse_record(const std::string& line) {
    Record record{line[0], {}};
    if (line.find('^') != std::string::npos) {
        std::stringstream stream(line.substr(line[1]=='^' ? 2 : 1));
        std::string field;
        while (std::getline(stream, field, '^')) record.fields.push_back(trim(field));
    } else {
        const std::string rest = line.substr(1);
        auto part = [&](size_t position, size_t size) { return trim(rest.substr(position, size)); };
        switch (record.type) {
            case 'H': record.fields = {part(0,6), part(6,6), part(12,6)}; break;
            case 'T': record.fields = {part(0,6), part(6,2), part(8,std::string::npos)}; break;
            case 'M': record.fields = {part(0,6), part(6,2), part(8,std::string::npos)}; break;
            case 'E': record.fields = {part(0,std::string::npos)}; break;
            default: throw std::runtime_error("Tipo de registro não suportado");
        }
    }
    return record;
}
}

SicXe::SicXe() : memory_(memory_size, 0) {}

uint8_t SicXe::byte(uint32_t address) const {
    if (address >= memory_size) throw std::runtime_error("Endereço fora da memória");
    return memory_[address];
}

uint32_t SicXe::word(uint32_t address) const {
    if (address > memory_size - 3) throw std::runtime_error("Palavra fora da memória");
    return (uint32_t(memory_[address]) << 16) | (uint32_t(memory_[address+1]) << 8) | memory_[address+2];
}

void SicXe::put_byte(uint32_t address, uint8_t value) {
    if (address >= memory_size) throw std::runtime_error("Escrita fora da memória");
    memory_[address] = value;
}

void SicXe::put_word(uint32_t address, uint32_t value) {
    if (address > memory_size - 3) throw std::runtime_error("Escrita de palavra fora da memória");
    for (int i = 0; i < 3; ++i) memory_[address+i] = uint8_t(value >> (16-8*i));
}

uint64_t SicXe::read48(uint32_t address) const {
    if (address > memory_size - 6) throw std::runtime_error("Float fora da memória");
    uint64_t value = 0;
    for (int i = 0; i < 6; ++i) value = (value << 8) | memory_[address+i];
    return value;
}

void SicXe::write48(uint32_t address, uint64_t value) {
    if (address > memory_size - 6) throw std::runtime_error("Escrita de float fora da memória");
    for (int i = 5; i >= 0; --i) { memory_[address+i] = uint8_t(value); value >>= 8; }
}

int32_t SicXe::signed24(uint32_t value) { return (value & 0x800000) ? int32_t(value | 0xff000000u) : int32_t(value); }
uint32_t SicXe::mask24(int64_t value) { return uint32_t(value) & word_mask; }

void SicXe::compare(int64_t left, int64_t right) {
    condition_ = (left > right) - (left < right);
    registers_[SW] = condition_ < 0 ? 0 : condition_ == 0 ? 1 : 2;
}

double SicXe::decode_float(uint64_t bits) const {
    if ((bits & 0xfffffffffULL) == 0) return 0;
    const int exponent = int((bits >> 36) & 0x7ff) - 1024;
    const double fraction = std::ldexp(double(bits & 0xfffffffffULL), -36);
    return std::ldexp((bits & (1ULL << 47)) ? -fraction : fraction, exponent);
}

uint64_t SicXe::encode_float(double value) const {
    if (value == 0) return 0;
    if (!std::isfinite(value)) throw std::runtime_error("Resultado de ponto flutuante inválido");
    int exponent = 0;
    const double fraction = std::frexp(std::abs(value), &exponent);
    if (exponent < -1024 || exponent > 1023) throw std::runtime_error("Expoente de ponto flutuante fora do limite");
    const uint64_t mantissa = uint64_t(std::ldexp(fraction, 36));
    return (std::signbit(value) ? 1ULL << 47 : 0) | (uint64_t(exponent+1024) << 36) | (mantissa & 0xfffffffffULL);
}

void SicXe::load(const std::string& object, uint32_t address) {
    if (address >= memory_size) throw std::runtime_error("Endereço de carga fora da memória");
    std::vector<Record> records;
    std::istringstream stream(object);
    std::string line;
    while (std::getline(stream, line)) {
        line = trim(line);
        if (!line.empty() && line[0] != '.') records.push_back(parse_record(line));
    }
    if (records.size() < 2 || records.front().type != 'H') throw std::runtime_error("Programa precisa começar com registro H");
    const auto& header = records.front().fields;
    if (header.size() < 3) throw std::runtime_error("Registro H incompleto");
    const uint32_t origin = hex(header[1]), length = hex(header[2]);
    if (length == 0 || uint64_t(address)+length > memory_size) throw std::runtime_error("Programa não cabe na memória");
    auto fresh = std::vector<uint8_t>(memory_size, 0);
    bool ended = false;
    uint32_t entry = address;
    const int64_t delta = int64_t(address) - origin;
    auto relocated = [&](uint32_t old) -> uint32_t {
        const int64_t new_address = int64_t(old) + delta;
        if (old < origin || uint64_t(old) >= uint64_t(origin)+length || new_address < 0 || new_address >= memory_size)
            throw std::runtime_error("Endereço de registro fora da seção de controle");
        return uint32_t(new_address);
    };
    for (size_t index=1; index<records.size(); ++index) {
        const auto& record = records[index];
        if (ended) throw std::runtime_error("Registro após E");
        if (record.type == 'T') {
            if (record.fields.size() < 3) throw std::runtime_error("Registro T incompleto");
            const uint32_t original = hex(record.fields[0]), size = hex(record.fields[1]);
            const std::string data = record.fields[2];
            if (size > 30 || data.size() != size*2 || uint64_t(original)+size > uint64_t(origin)+length)
                throw std::runtime_error("Tamanho inválido no registro T");
            const uint32_t target = relocated(original);
            for (uint32_t i=0; i<size; ++i) fresh[target+i] = uint8_t(hex(data.substr(i*2,2)));
        } else if (record.type == 'M') {
            // Applied in a second pass, after all T records have been loaded.
            continue;
        } else if (record.type == 'E') {
            if (!record.fields.empty() && !record.fields[0].empty()) entry = relocated(hex(record.fields[0]));
            ended = true;
        } else throw std::runtime_error("Registro objeto não suportado");
    }
    if (!ended) throw std::runtime_error("Registro E ausente");
    for (const auto& record : records) if (record.type == 'M') {
        if (record.fields.size() < 2) throw std::runtime_error("Registro M incompleto");
        const uint32_t at = relocated(hex(record.fields[0]));
        const std::string width = record.fields[1].substr(0,2);
        const std::string expression = record.fields[1].size()>2 ? record.fields[1].substr(2) :
            record.fields.size()>2 ? record.fields[2] : "";
        const uint32_t digits = hex(width);
        if (digits == 0 || digits > 6 || uint64_t(at)+(digits+1)/2 > uint64_t(address)+length)
            throw std::runtime_error("Campo inválido no registro M");
        int64_t adjustment = delta;
        if (!expression.empty()) {
            const std::string symbol = expression;
            if (symbol[0] != '+' && symbol[0] != '-') throw std::runtime_error("Expressão M inválida");
            if (trim(symbol.substr(1)) != trim(header[0])) throw std::runtime_error("Símbolo externo não definido: " + symbol);
            if (symbol[0] == '-') adjustment = -delta;
        }
        const uint32_t bytes = (digits+1)/2;
        uint64_t raw = 0;
        for (uint32_t i=0; i<bytes; ++i) raw = (raw<<8) | fresh[at+i];
        const uint32_t bits = digits*4;
        const uint64_t mask = (1ULL<<bits)-1;
        const uint64_t upper = raw & ~mask;
        raw = upper | ((raw + adjustment) & mask);
        for (uint32_t i=bytes; i>0; --i) { fresh[at+i-1] = uint8_t(raw); raw >>= 8; }
    }
    memory_ = std::move(fresh);
    object_ = object;
    name_ = header[0];
    start_ = address; length_ = length; entry_ = entry; load_address_ = address;
    registers_.fill(0);
    registers_[L] = 0xfffff; // RSUB at the top level stops execution.
    registers_[PC] = entry_;
    floating_ = 0; condition_ = 0; halted_ = false; error_.clear(); output_.clear(); input_.clear(); steps_ = 0;
}

void SicXe::reset() {
    if (!object_.empty()) load(object_, load_address_);
}

void SicXe::add_input(const std::string& data) {
    for (unsigned char ch : data) input_.push_back(ch);
}

size_t SicXe::run(size_t limit, int32_t breakpoint) {
    size_t count = 0;
    while (!halted_ && count < limit) {
        if (breakpoint >= 0 && registers_[PC] == uint32_t(breakpoint)) break;
        step(); ++count;
    }
    return count;
}

void SicXe::step() {
    if (halted_) return;
    try {
        const uint32_t pc = registers_[PC];
        if (pc == 0xfffff) { halted_ = true; return; }
        const uint8_t first = byte(pc);
        const uint8_t opcode = first & 0xfc;
        auto register_ref = [&](uint8_t number) -> uint32_t& {
            if (number > 9 || number == 6 || number == 7) throw std::runtime_error("Número de registrador inválido");
            return registers_[number];
        };
        const bool format1 = first==0xc0 || first==0xc4 || first==0xc8 || first==0xf0 || first==0xf4 || first==0xf8;
        const bool format2 = first==0x90 || first==0x94 || first==0x98 || first==0x9c || first==0xa0 || first==0xa4 || first==0xa8 || first==0xac || first==0xb0 || first==0xb4 || first==0xb8;
        if (format1) {
            registers_[PC] = pc+1;
            switch (first) {
                case 0xc0: floating_ = encode_float(signed24(registers_[A])); break;
                case 0xc4: registers_[A] = mask24(int64_t(decode_float(floating_))); break;
                case 0xc8: floating_ = encode_float(decode_float(floating_)); break;
                case 0xf0: case 0xf4: throw std::runtime_error("Canais de E/S não suportados");
                case 0xf8: compare(-1,0); break;
            }
        } else if (format2) {
            const uint8_t second = byte(pc+1), r1 = second>>4, r2 = second&15;
            registers_[PC] = pc+2;
            auto signed_reg = [&](uint8_t number) { return signed24(register_ref(number)); };
            switch (first) {
                case 0x90: register_ref(r2) = mask24(int64_t(signed_reg(r2))+signed_reg(r1)); break;
                case 0x94: register_ref(r2) = mask24(int64_t(signed_reg(r2))-signed_reg(r1)); break;
                case 0x98: register_ref(r2) = mask24(int64_t(signed_reg(r2))*signed_reg(r1)); break;
                case 0x9c: if (signed_reg(r1)==0) throw std::runtime_error("Divisão por zero"); register_ref(r2) = mask24(signed_reg(r2)/signed_reg(r1)); break;
                case 0xa0: compare(signed_reg(r1), signed_reg(r2)); break;
                case 0xa4: { const uint32_t value = register_ref(r1); const int n = r2+1; register_ref(r1) = mask24((value<<n) | (value>>(24-n))); break; }
                case 0xa8: register_ref(r1) = mask24(signed_reg(r1) >> (r2+1)); break;
                case 0xac: register_ref(r2) = register_ref(r1); break;
                case 0xb0: throw std::runtime_error("Interrupção SVC não suportada");
                case 0xb4: register_ref(r1) = 0; break;
                case 0xb8: registers_[X] = mask24(registers_[X]+1); compare(signed24(registers_[X]), signed_reg(r1)); break;
            }
        } else {
            // Reject undefined opcodes before treating arbitrary memory as instructions.
            switch (opcode) {
                case 0x00: case 0x04: case 0x08: case 0x0c: case 0x10: case 0x14:
                case 0x18: case 0x1c: case 0x20: case 0x24: case 0x28: case 0x2c:
                case 0x30: case 0x34: case 0x38: case 0x3c: case 0x40: case 0x44:
                case 0x48: case 0x4c: case 0x50: case 0x54: case 0x58: case 0x5c:
                case 0x60: case 0x64: case 0x68: case 0x6c: case 0x70: case 0x74:
                case 0x78: case 0x7c: case 0x80: case 0x84: case 0x88: case 0xd0:
                case 0xd4: case 0xd8: case 0xdc: case 0xe0: case 0xe8: case 0xec: break;
                default: throw std::runtime_error("Opcode desconhecido");
            }
            const uint8_t second = byte(pc+1), third = byte(pc+2);
            const bool n = first&2, i = first&1, x = second&0x80, b = second&0x40, p = second&0x20, e = second&0x10;
            const bool sic = !n && !i;
            const uint32_t size = (!sic && e) ? 4 : 3;
            const uint32_t next = pc+size;
            if (next > memory_size) throw std::runtime_error("Instrução fora da memória");
            registers_[PC] = next;
            if (!sic && ((b&&p) || (e&&(b||p)) || (x && n!=i))) throw std::runtime_error("Combinação de flags de endereçamento inválida");
            uint32_t target = 0;
            if (sic) target = ((uint32_t(second)&0x7f)<<8) | third;
            else {
                const uint32_t field = (uint32_t(second&15)<<8) | third;
                if (e) target = (field<<8) | byte(pc+3);
                else if (p) target = mask24(int64_t(next) + (field&0x800 ? int32_t(field)-4096 : int32_t(field)));
                else if (b) target = mask24(uint64_t(registers_[B])+field);
                else target = field;
            }
            if (x) target = mask24(uint64_t(target)+registers_[X]);
            if (n&&!i) target = word(target);
            const bool immediate = !n && i;
            auto value = [&]() -> uint32_t { return immediate ? target : word(target); };
            auto signed_value = [&]() -> int32_t { return signed24(value()); };
            auto float_value = [&]() -> double { return decode_float(read48(target)); };
            switch (opcode) {
                case 0x00: registers_[A] = value(); break;
                case 0x04: registers_[X] = value(); break;
                case 0x08: registers_[L] = value(); break;
                case 0x68: registers_[B] = value(); break;
                case 0x6c: registers_[S] = value(); break;
                case 0x74: registers_[T] = value(); break;
                case 0x70: floating_ = immediate ? encode_float(signed24(target)) : read48(target); break;
                case 0x50: registers_[A] = (registers_[A]&0xffff00) | (immediate ? target&255 : byte(target)); break;
                case 0x0c: case 0x10: case 0x14: case 0x78: case 0x7c: case 0x84: case 0xe8:
                    if (immediate) throw std::runtime_error("STORE não aceita modo imediato");
                    put_word(target, registers_[opcode==0x0c?A:opcode==0x10?X:opcode==0x14?L:opcode==0x78?B:opcode==0x7c?S:opcode==0x84?T:SW]); break;
                case 0x80: if (immediate) throw std::runtime_error("STF não aceita modo imediato"); write48(target,floating_); break;
                case 0x54: if (immediate) throw std::runtime_error("STCH não aceita modo imediato"); put_byte(target,uint8_t(registers_[A])); break;
                case 0x18: registers_[A] = mask24(int64_t(signed24(registers_[A]))+signed_value()); break;
                case 0x1c: registers_[A] = mask24(int64_t(signed24(registers_[A]))-signed_value()); break;
                case 0x20: registers_[A] = mask24(int64_t(signed24(registers_[A]))*signed_value()); break;
                case 0x24: if (signed_value()==0) throw std::runtime_error("Divisão por zero"); registers_[A] = mask24(signed24(registers_[A])/signed_value()); break;
                case 0x28: compare(signed24(registers_[A]), signed_value()); break;
                case 0x2c: registers_[X] = mask24(registers_[X]+1); compare(signed24(registers_[X]),signed_value()); break;
                case 0x40: registers_[A] &= value(); break;
                case 0x44: registers_[A] |= value(); break;
                case 0x3c: registers_[PC] = target; break;
                case 0x30: if (condition_==0) registers_[PC] = target; break;
                case 0x34: if (condition_>0) registers_[PC] = target; break;
                case 0x38: if (condition_<0) registers_[PC] = target; break;
                case 0x48: registers_[L] = next; registers_[PC] = target; break;
                case 0x4c: registers_[PC] = registers_[L]; if (registers_[PC]==0xfffff) halted_ = true; break;
                case 0x58: floating_ = encode_float(decode_float(floating_)+float_value()); break;
                case 0x5c: floating_ = encode_float(decode_float(floating_)-float_value()); break;
                case 0x60: floating_ = encode_float(decode_float(floating_)*float_value()); break;
                case 0x64: if (float_value()==0) throw std::runtime_error("Divisão por zero"); floating_ = encode_float(decode_float(floating_)/float_value()); break;
                case 0x88: { const double left=decode_float(floating_), right=float_value(); compare((left>right)-(left<right),0); break; }
                case 0xd8: if (input_.empty()) throw std::runtime_error("Entrada vazia para RD"); registers_[A] = (registers_[A]&0xffff00)|input_.front(); input_.pop_front(); break;
                case 0xdc: output_.push_back(char(registers_[A]&255)); break;
                case 0xe0: compare(input_.empty()?0:-1,0); break;
                case 0xd0: case 0xd4: case 0xec: throw std::runtime_error("Instrução privilegiada não suportada");
            }
        }
        ++steps_;
    } catch (const std::exception& exception) {
        halted_ = true;
        error_ = exception.what();
    }
}
