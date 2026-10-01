#include "sicxe.hpp"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>

namespace {
// Campos de registros objeto são sempre hexadecimais, sem prefixo 0x.
// Acumular em 64 bits permite detectar excesso antes de devolver uint32_t.
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

// Campos fixos podem conter espaços de preenchimento; linhas também vêm com
// quebras CRLF. A limpeza aqui permite tratar ambos os formatos do mesmo modo.
std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

// Mantém o tipo do registro e seus campos sem interpretar ainda o conteúdo.
struct Record { char type; std::vector<std::string> fields; };
// No formato delimitado, ^ separa os campos. No fixo, as posições seguem as
// larguras de H/T/M/E: endereço de seis dígitos, tamanho de dois em T/M.
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

// A máquina começa sem programa e com CC='=' refletido em SW.
SicXe::SicXe() : memory_(memory_size, 0) { compare(0,0); }

uint8_t SicXe::byte(uint32_t address) const {
    // Instruções de byte e busca de instruções passam pela mesma proteção.
    if (address >= memory_size) throw std::runtime_error("Endereço fora da memória");
    return memory_[address];
}

uint32_t SicXe::word(uint32_t address) const {
    // Palavra big-endian: m é o byte mais significativo, seguido por m+1/m+2.
    // A verificação considera os três endereços, não apenas o primeiro.
    if (address > memory_size - 3) throw std::runtime_error("Palavra fora da memória");
    return (uint32_t(memory_[address]) << 16) | (uint32_t(memory_[address+1]) << 8) | memory_[address+2];
}

void SicXe::put_byte(uint32_t address, uint8_t value) {
    // STCH precisa de uma escrita isolada; palavras usam put_word abaixo.
    if (address >= memory_size) throw std::runtime_error("Escrita fora da memória");
    memory_[address] = value;
}

void SicXe::put_word(uint32_t address, uint32_t value) {
    // Confere o intervalo antes da primeira escrita para evitar palavra parcial.
    // Os deslocamentos 16, 8 e 0 preservam a mesma ordem da leitura.
    if (address > memory_size - 3) throw std::runtime_error("Escrita de palavra fora da memória");
    for (int i = 0; i < 3; ++i) memory_[address+i] = uint8_t(value >> (16-8*i));
}

// C++ armazena os registradores inteiros em uint32_t; signed24 interpreta seu
// bit 23 como sinal. mask24 descarta qualquer bit além da palavra de 24 bits.
int32_t SicXe::signed24(uint32_t value) { return (value & 0x800000) ? int32_t(value) - 0x1000000 : int32_t(value); }
uint32_t SicXe::mask24(int64_t value) { return uint32_t(value) & word_mask; }

void SicXe::compare(int64_t left, int64_t right) {
    // Desvios consultam condition_; a interface também mostra SW. Por isso
    // ambos são atualizados juntos. Preservamos os demais bits de SW.
    condition_ = (left > right) - (left < right);
    registers_[SW] = (registers_[SW] & ~3u) | (condition_ < 0 ? 0 : condition_ == 0 ? 1 : 2);
}

void SicXe::load(const std::string& object, uint32_t address) {
    // Passo 1: separar registros e validar H. Tudo é montado em fresh; qualquer
    // exceção antes da troca final deixa memória e programa anteriores intactos.
    if (address >= memory_size) throw std::runtime_error("Endereço de carga fora da memória");
    std::vector<Record> records;
    std::istringstream stream(object);
    std::string line;
    while (std::getline(stream, line)) {
        line = trim(line);
        // Linhas vazias e comentários de programas objeto (prefixo '.') não
        // participam da carga.
        if (!line.empty() && line[0] != '.') records.push_back(parse_record(line));
    }
    if (records.size() < 2 || records.front().type != 'H') throw std::runtime_error("Programa precisa começar com registro H");
    const auto& header = records.front().fields;
    if (header.size() < 3) throw std::runtime_error("Registro H incompleto");
    const uint32_t origin = hex(header[1]), length = hex(header[2]);
    // H informa origem e comprimento da seção; address é onde ela ficará agora.
    if (length == 0 || uint64_t(address)+length > memory_size) throw std::runtime_error("Programa não cabe na memória");
    auto fresh = std::vector<uint8_t>(memory_size, 0);
    bool ended = false;
    uint32_t entry = address;
    const int64_t delta = int64_t(address) - origin;
    // Relocação: novo = endereço original + (carga - origem). A checagem exige
    // que o endereço original pertença à seção indicada pelo cabeçalho.
    auto relocated = [&](uint32_t old) -> uint32_t {
        const int64_t new_address = int64_t(old) + delta;
        if (old < origin || uint64_t(old) >= uint64_t(origin)+length || new_address < 0 || new_address >= memory_size)
            throw std::runtime_error("Endereço de registro fora da seção de controle");
        return uint32_t(new_address);
    };
    // Passo 2: copiar bytes T para fresh e escolher o ponto de entrada E.
    // Um M pode vir antes de T; por isso sua alteração espera a próxima passagem.
    for (size_t index=1; index<records.size(); ++index) {
        const auto& record = records[index];
        if (ended) throw std::runtime_error("Registro após E");
        if (record.type == 'T') {
            // Um T contém endereço, quantidade de bytes e seus dígitos hex.
            // O objeto pode repartir os dados em vários campos separados por ^.
            if (record.fields.size() < 3) throw std::runtime_error("Registro T incompleto");
            const uint32_t original = hex(record.fields[0]), size = hex(record.fields[1]);
            std::string data;
            for (size_t field=2; field<record.fields.size(); ++field) data += record.fields[field];
            if (size > 30 || data.size() != size*2 || uint64_t(original)+size > uint64_t(origin)+length)
                throw std::runtime_error("Tamanho inválido no registro T");
            const uint32_t target = relocated(original);
            for (uint32_t i=0; i<size; ++i) fresh[target+i] = uint8_t(hex(data.substr(i*2,2)));
        } else if (record.type == 'M') {
            // M é aplicado depois, quando todos os bytes já estão presentes.
            continue;
        } else if (record.type == 'E') {
            // E pode omitir o endereço: nesse caso a entrada é a posição de carga.
            if (!record.fields.empty() && !record.fields[0].empty()) entry = relocated(hex(record.fields[0]));
            ended = true;
        } else throw std::runtime_error("Registro objeto não suportado");
    }
    if (!ended) throw std::runtime_error("Registro E ausente");
    // Passo 3: aplicar cada M sobre os bytes de fresh. A largura é medida em
    // nibbles; o caso comum de cinco nibbles altera 20 bits de uma instrução
    // de formato 4 e preserva as flags no nibble superior.
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
            // Esta versão só aceita referência à própria seção; '+' e '-'
            // determinam o sentido do ajuste. Símbolos externos não são ligados.
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
        // upper conserva bits fora do campo M; a máscara limita o ajuste ao
        // número de nibbles solicitado, mesmo se ocorrer retorno módulo 2^bits.
        const uint64_t upper = raw & ~mask;
        raw = upper | ((raw + adjustment) & mask);
        for (uint32_t i=bytes; i>0; --i) { fresh[at+i-1] = uint8_t(raw); raw >>= 8; }
    }
    // Passo 4: somente após validar todos os registros, publicar memória,
    // metadados e estado inicial. Isso também descarta a carga anterior.
    memory_ = std::move(fresh);
    object_ = object;
    name_ = header[0];
    start_ = address; length_ = length; entry_ = entry; load_address_ = address;
    registers_.fill(0);
    // Uma sub-rotina retorna pelo endereço salvo em L. O valor inicial é uma
    // sentinela: RSUB no programa principal encerra sem buscar outra instrução.
    registers_[L] = 0xfffff;
    registers_[PC] = entry_;
    floating_ = 0; compare(0,0); halted_ = false; error_.clear(); steps_ = 0;
}

void SicXe::reset() {
    // Reutiliza o mesmo caminho de validação da carga para restaurar memória
    // e registradores, inclusive depois de instruções que alteraram o programa.
    if (!object_.empty()) load(object_, load_address_);
}

size_t SicXe::run(size_t limit, int32_t breakpoint) {
    // O breakpoint é testado antes da busca: a instrução nesse PC não é executada.
    // count mede tentativas de step; steps_ mede apenas instruções concluídas.
    size_t count = 0;
    while (!halted_ && count < limit) {
        if (breakpoint >= 0 && registers_[PC] == uint32_t(breakpoint)) break;
        step(); ++count;
    }
    return count;
}

void SicXe::step() {
    if (halted_) return;
    // Salva o estado antes da busca. Se uma flag for inválida ou uma leitura
    // sair da memória, o erro não deixará registradores parcialmente alterados.
    const uint32_t pc = registers_[PC];
    const auto saved_registers = registers_;
    const int saved_condition = condition_;
    try {
        // RSUB principal transfere a sentinela para PC; ela marca término normal.
        if (pc == 0xfffff) { halted_ = true; return; }
        const uint8_t first = byte(pc);
        const uint8_t opcode = first & 0xfc;
        // Os dois bits baixos do primeiro byte são n/i; a máscara 0xFC obtém
        // o opcode da tabela. Instruções em vermelho são identificadas antes
        // de qualquer mudança de PC ou de registradores.
        const char* excluded = nullptr;
        switch (opcode) {
            case 0x58: excluded = "ADDF"; break;
            case 0x88: excluded = "COMPF"; break;
            case 0x64: excluded = "DIVF"; break;
            case 0xc4: excluded = "FIX"; break;
            case 0xc0: excluded = "FLOAT"; break;
            case 0xf4: excluded = "HIO"; break;
            case 0x70: excluded = "LDF"; break;
            case 0xd0: excluded = "LPS"; break;
            case 0x60: excluded = "MULF"; break;
            case 0xc8: excluded = "NORM"; break;
            case 0xd8: excluded = "RD"; break;
            case 0xf0: excluded = "SIO"; break;
            case 0xec: excluded = "SSK"; break;
            case 0x80: excluded = "STF"; break;
            case 0xd4: excluded = "STI"; break;
            case 0xe8: excluded = "STSW"; break;
            case 0x5c: excluded = "SUBF"; break;
            case 0xb0: excluded = "SVC"; break;
            case 0xe0: excluded = "TD"; break;
            case 0xf8: excluded = "TIO"; break;
            case 0xdc: excluded = "WD"; break;
        }
        if (excluded) throw std::runtime_error(std::string(excluded) + ": instrução excluída pelo enunciado (em vermelho)");
        // Formato 2 usa números de registrador. F (6) é de 48 bits e não
        // participa destas operações inteiras; 7 não corresponde a registrador.
        auto register_ref = [&](uint8_t number) -> uint32_t& {
            if (number > 9 || number == 6 || number == 7) throw std::runtime_error("Número de registrador inválido");
            return registers_[number];
        };
        const bool format2 = first==0x90 || first==0x94 || first==0x98 || first==0x9c || first==0xa0 || first==0xa4 || first==0xa8 || first==0xac || first==0xb4 || first==0xb8;
        if (format2) {
            // Formato 2 ocupa dois bytes: o segundo reúne r1 no nibble alto e
            // r2 no baixo. Para operações binárias, r2 recebe o resultado.
            const uint8_t second = byte(pc+1), r1 = second>>4, r2 = second&15;
            registers_[PC] = pc+2;
            // Cálculo e comparação tratam os 24 bits como inteiro com sinal;
            // a escrita volta a limitar o resultado a 24 bits.
            auto signed_reg = [&](uint8_t number) { return signed24(register_ref(number)); };
            switch (first) {
                // ADDR/SUBR/MULR/DIVR: r2 op r1 -> r2.
                case 0x90: register_ref(r2) = mask24(int64_t(signed_reg(r2))+signed_reg(r1)); break;
                case 0x94: register_ref(r2) = mask24(int64_t(signed_reg(r2))-signed_reg(r1)); break;
                case 0x98: register_ref(r2) = mask24(int64_t(signed_reg(r2))*signed_reg(r1)); break;
                case 0x9c: if (signed_reg(r1)==0) throw std::runtime_error("Divisão por zero"); register_ref(r2) = mask24(signed_reg(r2)/signed_reg(r1)); break;
                // COMPR só atualiza o código condicional.
                case 0xa0: compare(signed_reg(r1), signed_reg(r2)); break;
                // O nibble r2 codifica n-1: desloca de 1 a 16 posições.
                // SHIFTL faz rotação nos 24 bits; SHIFTR replica o bit de sinal.
                case 0xa4: { const uint32_t value = register_ref(r1); const int n = r2+1; register_ref(r1) = mask24((value<<n) | (value>>(24-n))); break; }
                case 0xa8: { const uint32_t value = register_ref(r1); const int n = r2+1;
                    register_ref(r1) = (value >> n) | ((value & 0x800000) ? (word_mask << (24-n)) & word_mask : 0); break; }
                // RMO copia; CLEAR zera; TIXR incrementa X e compara com r1.
                case 0xac: register_ref(r2) = register_ref(r1); break;
                case 0xb4: register_ref(r1) = 0; break;
                case 0xb8: registers_[X] = mask24(registers_[X]+1); compare(signed24(registers_[X]), signed_reg(r1)); break;
            }
            // Instruções como CLEAR SW podem escrever SW diretamente. O CC
            // separado é sincronizado com seus bits baixos após o formato 2.
            const uint32_t cc = registers_[SW] & 3;
            condition_ = cc == 0 ? -1 : cc == 2 ? 1 : 0;
        } else {
            // Os formatos 3/4 e o SIC clássico partilham esta decodificação.
            // Rejeitar primeiro os opcodes desconhecidos evita tratar qualquer
            // byte de memória como se fosse uma instrução válida.
            switch (opcode) {
                case 0x00: case 0x04: case 0x08: case 0x0c: case 0x10: case 0x14:
                case 0x18: case 0x1c: case 0x20: case 0x24: case 0x28: case 0x2c:
                case 0x30: case 0x34: case 0x38: case 0x3c: case 0x40: case 0x44:
                case 0x48: case 0x4c: case 0x50: case 0x54:
                case 0x68: case 0x6c: case 0x74:
                case 0x78: case 0x7c: case 0x84: break;
                default: throw std::runtime_error("Opcode desconhecido");
            }
            const uint8_t second = byte(pc+1), third = byte(pc+2);
            // n/i: 11 direto, 10 indireto, 01 imediato, 00 SIC clássico.
            // x adiciona X; b usa B; p usa PC; e estende para quatro bytes.
            const bool n = first&2, i = first&1, x = second&0x80, b = second&0x40, p = second&0x20, e = second&0x10;
            const bool sic = !n && !i;
            const uint32_t size = (!sic && e) ? 4 : 3;
            const uint32_t next = pc+size;
            // PC avança antes do cálculo: deslocamento relativo usa o endereço
            // da próxima instrução, não o endereço do opcode atual.
            if (next > memory_size) throw std::runtime_error("Instrução fora da memória");
            registers_[PC] = next;
            // b e p não podem estar juntos; formato 4 não combina com b/p;
            // indexação não é admitida nos modos indireto e imediato.
            if (!sic && ((b&&p) || (e&&(b||p)) || (x && n!=i))) throw std::runtime_error("Combinação de flags de endereçamento inválida");
            uint32_t target = 0;
            // SIC clássico usa 15 bits de endereço: o bit alto do segundo byte
            // é x, e o bit que seria e no XE faz parte do endereço.
            if (sic) target = ((uint32_t(second)&0x7f)<<8) | third;
            else {
                // Campo de 12 bits no formato 3; e=1 acrescenta oito bits.
                // Em p=1, o bit 11 é o sinal do deslocamento em complemento
                // de dois. Em b=1, o deslocamento a partir de B é sem sinal.
                const uint32_t field = (uint32_t(second&15)<<8) | third;
                if (e) target = (field<<8) | byte(pc+3);
                else if (p) target = mask24(int64_t(next) + (field&0x800 ? int32_t(field)-4096 : int32_t(field)));
                else if (b) target = mask24(uint64_t(registers_[B])+field);
                else target = field;
            }
            // Indexação é somada ao endereço direto antes da indireção.
            if (x) target = mask24(uint64_t(target)+registers_[X]);
            // Indireto lê uma palavra que contém o endereço final. Imediato
            // usa o endereço calculado como valor, sem ler uma palavra nele.
            if (n&&!i) target = word(target);
            const bool immediate = !n && i;
            auto value = [&]() -> uint32_t { return immediate ? target : word(target); };
            auto signed_value = [&]() -> int32_t { return signed24(value()); };
            // Cada opcode abaixo consome o mesmo endereço/valor já calculado.
            switch (opcode) {
                // Cargas de palavra para os seis registradores inteiros.
                case 0x00: registers_[A] = value(); break;
                case 0x04: registers_[X] = value(); break;
                case 0x08: registers_[L] = value(); break;
                case 0x68: registers_[B] = value(); break;
                case 0x6c: registers_[S] = value(); break;
                case 0x74: registers_[T] = value(); break;
                // LDCH substitui só o byte direito de A; os outros 16 ficam.
                case 0x50: registers_[A] = (registers_[A]&0xffff00) | (immediate ? target&255 : byte(target)); break;
                // Armazenamento exige endereço de memória; o modo imediato não
                // fornece local de escrita. STCH escreve apenas um byte.
                case 0x0c: case 0x10: case 0x14: case 0x78: case 0x7c: case 0x84:
                    if (immediate) throw std::runtime_error("STORE não aceita modo imediato");
                    put_word(target, registers_[opcode==0x0c?A:opcode==0x10?X:opcode==0x14?L:opcode==0x78?B:opcode==0x7c?S:T]); break;
                case 0x54: if (immediate) throw std::runtime_error("STCH não aceita modo imediato"); put_byte(target,uint8_t(registers_[A])); break;
                // Operações aritméticas interpretam operandos como assinados,
                // rejeitam divisor zero e reduzem os resultados a 24 bits.
                case 0x18: registers_[A] = mask24(int64_t(signed24(registers_[A]))+signed_value()); break;
                case 0x1c: registers_[A] = mask24(int64_t(signed24(registers_[A]))-signed_value()); break;
                case 0x20: registers_[A] = mask24(int64_t(signed24(registers_[A]))*signed_value()); break;
                case 0x24: if (signed_value()==0) throw std::runtime_error("Divisão por zero"); registers_[A] = mask24(signed24(registers_[A])/signed_value()); break;
                // COMP e TIX ajustam CC; TIX incrementa X antes de comparar.
                case 0x28: compare(signed24(registers_[A]), signed_value()); break;
                case 0x2c: registers_[X] = mask24(registers_[X]+1); compare(signed24(registers_[X]),signed_value()); break;
                // AND/OR atuam bit a bit nos 24 bits do acumulador.
                case 0x40: registers_[A] &= value(); break;
                case 0x44: registers_[A] |= value(); break;
                // J desvia sempre; JEQ/JGT/JLT só substituem PC quando CC
                // corresponde à condição pedida.
                case 0x3c: registers_[PC] = target; break;
                case 0x30: if (condition_==0) registers_[PC] = target; break;
                case 0x34: if (condition_>0) registers_[PC] = target; break;
                case 0x38: if (condition_<0) registers_[PC] = target; break;
                // JSUB salva o endereço de retorno em L. RSUB volta para L;
                // se L ainda tem a sentinela inicial, a execução termina.
                case 0x48: registers_[L] = next; registers_[PC] = target; break;
                case 0x4c: registers_[PC] = registers_[L]; if (registers_[PC]==0xfffff) halted_ = true; break;
            }
        }
        // Somente instruções executadas sem exceção entram na contagem.
        ++steps_;
    } catch (const std::exception& exception) {
        // Restaura PC, demais registradores e CC para que a interface aponte
        // exatamente a instrução que falhou. halted_ impede novos passos até
        // que o usuário carregue ou reinicie o programa.
        registers_ = saved_registers;
        condition_ = saved_condition;
        halted_ = true;
        error_ = exception.what();
    }
}
