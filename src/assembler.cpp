#include "assembler.hpp"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

// ============================================================================
// PARTE 1 - DEFINIÇÕES DA ARQUITETURA E FUNÇÕES AUXILIARES
// Contém a tabela de instruções, as diretivas reconhecidas e as conversões
// básicas usadas durante todo o processo de montagem.
// ============================================================================
namespace {
// Cada entrada da OPTAB informa o opcode, o formato original e a quantidade
// esperada de operandos da instrução.
struct Op { uint8_t code; uint8_t format; uint8_t operands; };

const std::map<std::string, Op>& operations() {
    static const std::map<std::string, Op> table = {
        {"ADD",{0x18,3,1}}, {"ADDR",{0x90,2,2}}, {"AND",{0x40,3,1}},
        {"CLEAR",{0xb4,2,1}}, {"COMP",{0x28,3,1}}, {"COMPR",{0xa0,2,2}},
        {"DIV",{0x24,3,1}}, {"DIVR",{0x9c,2,2}}, {"J",{0x3c,3,1}},
        {"JEQ",{0x30,3,1}}, {"JGT",{0x34,3,1}}, {"JLT",{0x38,3,1}},
        {"JSUB",{0x48,3,1}}, {"LDA",{0x00,3,1}}, {"LDB",{0x68,3,1}},
        {"LDCH",{0x50,3,1}}, {"LDL",{0x08,3,1}}, {"LDS",{0x6c,3,1}},
        {"LDT",{0x74,3,1}}, {"LDX",{0x04,3,1}}, {"MUL",{0x20,3,1}},
        {"MULR",{0x98,2,2}}, {"OR",{0x44,3,1}}, {"RMO",{0xac,2,2}},
        {"RSUB",{0x4c,3,0}}, {"SHIFTL",{0xa4,2,2}}, {"SHIFTR",{0xa8,2,2}},
        {"STA",{0x0c,3,1}}, {"STB",{0x78,3,1}}, {"STCH",{0x54,3,1}},
        {"STL",{0x14,3,1}}, {"STS",{0x7c,3,1}}, {"STT",{0x84,3,1}},
        {"STX",{0x10,3,1}}, {"SUB",{0x1c,3,1}}, {"SUBR",{0x94,2,2}},
        {"TIX",{0x2c,3,1}}, {"TIXR",{0xb8,2,1}}
    };
    return table;
}

const std::set<std::string>& directives() {
    static const std::set<std::string> values = {"START","END","BYTE","WORD","RESB","RESW","BASE","NOBASE"};
    return values;
}

// Remove espaços das extremidades sem modificar espaços internos, importantes
// em constantes como BYTE C'OLA MUNDO'.
std::string trim(std::string value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    return value.substr(begin, value.find_last_not_of(" \t\r\n")-begin+1);
}

// A montagem não diferencia maiúsculas de minúsculas em nomes e mnemônicos.
std::string upper(std::string value) {
    std::transform(value.begin(),value.end(),value.begin(),[](unsigned char c){ return char(std::toupper(c)); });
    return value;
}

// Produz os campos hexadecimais de largura fixa dos registros objeto.
std::string hex(uint32_t value, unsigned width) {
    std::ostringstream out;
    out << std::uppercase << std::hex << std::setfill('0') << std::setw(int(width)) << value;
    return out.str();
}

// Centraliza os diagnósticos para que todo erro relacionado ao fonte indique
// exatamente a linha onde foi encontrado.
[[noreturn]] void fail(size_t line, const std::string& message) {
    throw std::runtime_error("Linha " + std::to_string(line) + ": " + message);
}

// Converte números, aceitando a base esperada pelo contexto, prefixo 0x ou
// sufixo H para hexadecimal. O acúmulo em 64 bits permite detectar overflow.
uint32_t number(const std::string& text, int base, size_t line) {
    std::string value = trim(text);
    if (value.empty() || value[0]=='-') fail(line,"número inválido: " + text);
    int actual = base;
    if (value.size()>2 && value[0]=='0' && (value[1]=='x'||value[1]=='X')) { actual=16; value=value.substr(2); }
    else if (value.size()>1 && (value.back()=='H'||value.back()=='h')) { actual=16; value.pop_back(); }
    if (value.empty()) fail(line,"número inválido: " + text);
    uint64_t result=0;
    for (char ch : value) {
        const int digit = std::isdigit(static_cast<unsigned char>(ch)) ? ch-'0' :
            std::isalpha(static_cast<unsigned char>(ch)) ? std::toupper(static_cast<unsigned char>(ch))-'A'+10 : -1;
        if (digit<0 || digit>=actual) fail(line,"número inválido: " + text);
        result=result*actual+unsigned(digit);
        if (result>0xffffffffu) fail(line,"número grande demais: " + text);
    }
    return uint32_t(result);
}

// O prefixo '+' muda uma instrução de formato 3 para formato 4, mas não muda
// sua presença na tabela de operações.
bool known_operation(std::string token) {
    if (!token.empty() && token[0]=='+') token.erase(token.begin());
    token=upper(token);
    return operations().count(token) || directives().count(token);
}

// ============================================================================
// PARTE 2 - LEITURA E REPRESENTAÇÃO DO PROGRAMA FONTE
// Transforma o texto em linhas estruturadas e fornece rotinas para interpretar
// constantes, registradores, listas de operandos e símbolos.
// ============================================================================

// A linha mantém tanto o texto original, usado na listagem, quanto os campos
// normalizados e os valores calculados durante a primeira passagem.
struct Line {
    size_t number=0;
    std::string original, label, operation, operand;
    uint32_t address=0, size=0;
    bool comment=false;
};

// Se o primeiro termo não for uma instrução nem diretiva, ele é interpretado
// como rótulo. Linhas vazias e linhas iniciadas por ponto são comentários.
Line parse_line(const std::string& original, size_t line_number) {
    Line line; line.number=line_number; line.original=original;
    const std::string clean=trim(original);
    if (clean.empty() || clean[0]=='.') { line.comment=true; return line; }
    std::istringstream in(clean);
    std::vector<std::string> tokens;
    std::string token;
    while (in>>token) tokens.push_back(token);
    if (tokens.empty()) { line.comment=true; return line; }
    size_t op_index=0;
    if (!known_operation(tokens[0])) {
        line.label=upper(tokens[0]);
        op_index=1;
        if (tokens.size()==1) fail(line_number,"operação ausente");
    }
    line.operation=upper(tokens[op_index]);
    const size_t position=clean.find(tokens[op_index]);
    line.operand=trim(clean.substr(position+tokens[op_index].size()));
    return line;
}

// Converte BYTE C'...' diretamente em caracteres e BYTE X'...' em pares de
// dígitos hexadecimais.
std::vector<uint8_t> byte_value(const std::string& operand, size_t line) {
    if (operand.size()<3 || operand[1]!='\'' || operand.back()!='\'' || (std::toupper(static_cast<unsigned char>(operand[0]))!='C' && std::toupper(static_cast<unsigned char>(operand[0]))!='X'))
        fail(line,"BYTE exige C'...' ou X'...'");
    const char kind=char(std::toupper(static_cast<unsigned char>(operand[0])));
    const std::string contents=operand.substr(2,operand.size()-3);
    std::vector<uint8_t> bytes;
    if (kind=='C') for (unsigned char ch : contents) bytes.push_back(ch);
    else {
        if (contents.empty() || contents.size()%2) fail(line,"constante hexadecimal BYTE deve ter quantidade par de dígitos");
        for (size_t i=0;i<contents.size();i+=2) bytes.push_back(uint8_t(number(contents.substr(i,2),16,line)));
    }
    return bytes;
}

// Traduz o nome textual para o número de registrador definido pelo SIC/XE.
uint8_t register_number(const std::string& name, size_t line) {
    static const std::map<std::string,uint8_t> regs={{"A",0},{"X",1},{"L",2},{"B",3},{"S",4},{"T",5},{"F",6},{"PC",8},{"SW",9}};
    const auto found=regs.find(upper(trim(name)));
    if (found==regs.end()) fail(line,"registrador inválido: " + trim(name));
    return found->second;
}

// Separa operandos de formato 2, como ADDR A,X e SHIFTL A,4.
std::vector<std::string> operands(const std::string& text) {
    std::vector<std::string> result;
    std::stringstream stream(text);
    std::string item;
    while (std::getline(stream,item,',')) result.push_back(trim(item));
    if (result.size()==1 && result[0].empty()) result.clear();
    return result;
}

// Consulta primeiro a SYMTAB. Se o termo não for um símbolo, ele deve ser uma
// constante numérica decimal (ou usar uma notação hexadecimal explícita).
uint32_t resolve(const std::string& value, const std::map<std::string,uint32_t>& symbols, size_t line, bool& symbolic) {
    const std::string key=upper(trim(value));
    const auto found=symbols.find(key);
    if (found!=symbols.end()) { symbolic=true; return found->second; }
    symbolic=false;
    return number(key,10,line);
}

// Uma palavra SIC/XE ocupa três bytes em ordem big-endian.
void append_u24(std::vector<uint8_t>& bytes, uint32_t value) {
    bytes.push_back(uint8_t(value>>16)); bytes.push_back(uint8_t(value>>8)); bytes.push_back(uint8_t(value));
}

// Converte bytes já codificados na representação usada pelos registros T e
// pela coluna de código objeto da listagem.
std::string bytes_hex(const std::vector<uint8_t>& bytes) {
    std::string result;
    for (uint8_t byte : bytes) result += hex(byte,2);
    return result;
}
}

// ============================================================================
// PARTE 3 - MONTAGEM EM DUAS PASSAGENS
// A passagem 1 calcula LOCCTR e SYMTAB. A passagem 2 usa esses endereços para
// codificar instruções, selecionar os bits nixbpe e registrar relocações.
// ============================================================================
AssemblyResult Assembler::assemble(const std::string& source) const {
    // Antes das passagens, o texto é convertido para a representação Line.
    // Nenhum resultado parcial é exposto se alguma linha for inválida.
    std::vector<Line> lines;
    std::istringstream input(source);
    std::string raw;
    size_t line_number=0;
    while (std::getline(input,raw)) lines.push_back(parse_line(raw,++line_number));
    if (lines.empty()) throw std::runtime_error("Programa fonte vazio");

    // PASSAGEM 1: atribui endereços, valida tamanhos e cria a tabela de símbolos.
    std::map<std::string,uint32_t> symbols;
    uint32_t start=0, location=0;
    std::string program_name="NONAME";
    bool began=false, ended=false;
    for (auto& line : lines) {
        if (line.comment) continue;
        if (ended) fail(line.number,"instrução após END");
        if (!began) {
            began=true;
            if (line.operation=="START") {
                start=location=number(line.operand,16,line.number);
                if (start>=0x100000) fail(line.number,"endereço START fora da memória");
                line.address=start;
                if (!line.label.empty()) {
                    program_name=line.label;
                    symbols[line.label]=start;
                }
                continue;
            }
        } else if (line.operation=="START") fail(line.number,"START deve ser a primeira instrução");
        line.address=location;
        // O endereço atual é associado ao rótulo antes de LOCCTR avançar.
        if (!line.label.empty()) {
            if (symbols.count(line.label)) fail(line.number,"símbolo duplicado: " + line.label);
            symbols[line.label]=location;
            if (program_name=="NONAME" && symbols.size()==1) program_name=line.label;
        }
        std::string mnemonic=line.operation;
        bool extended=!mnemonic.empty() && mnemonic[0]=='+';
        if (extended) mnemonic.erase(mnemonic.begin());
        const auto op=operations().find(mnemonic);
        // Instruções avançam pelo próprio formato; diretivas de dados e reserva
        // avançam pela quantidade de bytes que ocupam ou reservam.
        if (op!=operations().end()) {
            if (extended && op->second.format!=3) fail(line.number,"formato 4 inválido para " + mnemonic);
            line.size=extended?4:op->second.format;
        } else if (line.operation=="WORD") line.size=3;
        else if (line.operation=="BYTE") line.size=uint32_t(byte_value(line.operand,line.number).size());
        else if (line.operation=="RESB") line.size=number(line.operand,10,line.number);
        else if (line.operation=="RESW") {
            const uint32_t count=number(line.operand,10,line.number);
            if (count>std::numeric_limits<uint32_t>::max()/3) fail(line.number,"RESW grande demais");
            line.size=count*3;
        } else if (line.operation=="BASE" || line.operation=="NOBASE") line.size=0;
        else if (line.operation=="END") { line.size=0; ended=true; }
        else fail(line.number,"operação desconhecida: " + line.operation);
        if (uint64_t(location)+line.size>0x100000) fail(line.number,"programa ultrapassa a memória");
        location+=line.size;
    }
    if (!began) throw std::runtime_error("Programa fonte vazio");
    if (!ended) throw std::runtime_error("Diretiva END ausente");
    const uint32_t length=location-start;
    if (length==0) throw std::runtime_error("Programa não gera área de memória");

    // PASSAGEM 2: traduz cada linha usando a SYMTAB completa.
    std::vector<std::vector<uint8_t>> codes(lines.size());
    std::vector<uint32_t> modifications;
    bool base_set=false; uint32_t base=0; uint32_t entry=start;
    for (size_t index=0;index<lines.size();++index) {
        const Line& line=lines[index];
        if (line.comment || line.operation=="START") continue;
        if (line.operation=="END") {
            if (!line.operand.empty()) { bool symbolic=false; entry=resolve(line.operand,symbols,line.number,symbolic); }
            continue;
        }
        if (line.operation=="BASE") {
            // BASE não gera bytes: apenas habilita endereçamento base-relativo
            // para as instruções seguintes, até que NOBASE seja encontrado.
            if (line.operand.empty()) fail(line.number,"operando BASE ausente");
            bool symbolic=false; base=resolve(line.operand,symbols,line.number,symbolic); base_set=true; continue;
        }
        if (line.operation=="NOBASE") { if (!line.operand.empty()) fail(line.number,"NOBASE não recebe operando"); base_set=false; continue; }
        if (line.operation=="RESB" || line.operation=="RESW") continue;
        if (line.operation=="BYTE") { codes[index]=byte_value(line.operand,line.number); continue; }
        if (line.operation=="WORD") {
            std::string value=trim(line.operand);
            int64_t signed_value=0;
            if (!value.empty() && value[0]=='-') signed_value=-int64_t(number(value.substr(1),10,line.number));
            else { bool symbolic=false; signed_value=resolve(value,symbols,line.number,symbolic); }
            if (signed_value < -8388608 || signed_value > 0xffffff) fail(line.number,"WORD fora de 24 bits");
            append_u24(codes[index],uint32_t(signed_value)&0xffffff); continue;
        }

        std::string mnemonic=line.operation;
        const bool extended=mnemonic[0]=='+';
        if (extended) mnemonic.erase(mnemonic.begin());
        const Op op=operations().at(mnemonic);
        if (op.format==2) {
            // No formato 2, r1 ocupa o nibble alto e r2 o nibble baixo. Nas
            // instruções de deslocamento, o segundo nibble guarda n - 1.
            const auto args=operands(line.operand);
            if (args.size()!=op.operands) fail(line.number,"quantidade de operandos inválida para " + mnemonic);
            const uint8_t r1=register_number(args[0],line.number);
            uint8_t r2=0;
            if (args.size()==2) {
                if (mnemonic=="SHIFTL" || mnemonic=="SHIFTR") {
                    const uint32_t count=number(args[1],10,line.number);
                    if (count<1 || count>16) fail(line.number,"deslocamento deve estar entre 1 e 16");
                    r2=uint8_t(count-1);
                } else r2=register_number(args[1],line.number);
            }
            codes[index]={op.code,uint8_t((r1<<4)|r2)};
            continue;
        }
        if (mnemonic=="RSUB") {
            if (!trim(line.operand).empty()) fail(line.number,"RSUB não recebe operando");
            codes[index]={uint8_t(op.code|3),0,0}; continue;
        }

        std::string operand=trim(line.operand);
        bool n=true,i=true,x=false;
        // O modo simples começa com n=i=1. '#' seleciona imediato, '@'
        // indireto e o sufixo ',X' ativa indexação somente no modo simples.
        if (!operand.empty() && operand[0]=='#') { n=false; i=true; operand=trim(operand.substr(1)); }
        else if (!operand.empty() && operand[0]=='@') { n=true; i=false; operand=trim(operand.substr(1)); }
        const auto comma=operand.find(',');
        if (comma!=std::string::npos) {
            if (upper(trim(operand.substr(comma+1)))!="X" || !n || !i) fail(line.number,"indexação exige operando simples seguido de ,X");
            x=true; operand=trim(operand.substr(0,comma));
        }
        if (operand.empty()) fail(line.number,"operando ausente");
        bool symbolic=false;
        uint32_t target=0;
        if (!operand.empty() && operand[0]=='-') {
            if (n || !i) fail(line.number,"valor negativo só é válido no modo imediato");
            const uint32_t magnitude=number(operand.substr(1),10,line.number);
            const uint32_t limit=extended?0x80000:0x800;
            if (magnitude>limit) fail(line.number,"constante imediata negativa não cabe no formato escolhido");
            target=(0u-magnitude)&(extended?0xfffff:0xfff);
        } else target=resolve(operand,symbols,line.number,symbolic);
        const uint8_t first=uint8_t(op.code|(n?2:0)|(i?1:0));
        if (extended) {
            // Formato 4 possui endereço de 20 bits. Um símbolo relocável gera
            // depois um registro M começando no byte seguinte ao opcode.
            if (target>0xfffff) fail(line.number,"endereço não cabe no formato 4");
            codes[index]={first,uint8_t((x?0x80:0)|0x10|(target>>16)),uint8_t(target>>8),uint8_t(target)};
            if (symbolic) modifications.push_back(line.address+1);
        } else {
            uint16_t field=0; uint8_t flags=x?0x80:0;
            if (!symbolic) {
                if (target>0xfff) fail(line.number,"valor absoluto não cabe no formato 3; use formato 4");
                field=uint16_t(target);
            } else {
                // Para símbolos no formato 3, PC relativo é a primeira escolha;
                // BASE é usado somente quando o deslocamento de PC não cabe.
                const int64_t displacement=int64_t(target)-int64_t(line.address+3);
                if (displacement>=-2048 && displacement<=2047) { flags|=0x20; field=uint16_t(displacement)&0xfff; }
                else if (base_set && target>=base && target-base<=0xfff) { flags|=0x40; field=uint16_t(target-base); }
                else fail(line.number,"símbolo fora do alcance do formato 3; use +" + mnemonic + " ou BASE");
            }
            codes[index]={first,uint8_t(flags|(field>>8)),uint8_t(field)};
        }
    }

    // ========================================================================
    // PARTE 4 - GERAÇÃO DOS REGISTROS OBJETO E DA LISTAGEM
    // Empacota o resultado da segunda passagem em H/T/M/E e produz uma visão
    // legível com linha, endereço, código objeto e texto fonte original.
    // ========================================================================

    // O registro H descreve nome, endereço inicial e tamanho total da seção.
    std::ostringstream object;
    std::string header_name=program_name.substr(0,6);
    object << "H^" << header_name << '^' << hex(start,6) << '^' << hex(length,6) << '\n';
    std::vector<uint8_t> text; uint32_t text_start=0;
    // Cada registro T comporta no máximo 30 bytes contíguos. RESB e RESW
    // provocam uma quebra porque não produzem conteúdo no arquivo objeto.
    auto flush=[&]() {
        if (text.empty()) return;
        object << "T^" << hex(text_start,6) << '^' << hex(uint32_t(text.size()),2) << '^' << bytes_hex(text) << '\n';
        text.clear();
    };
    for (size_t index=0;index<lines.size();++index) {
        const auto& code=codes[index];
        if (code.empty()) { if (lines[index].size) flush(); continue; }
        size_t consumed=0;
        while (consumed<code.size()) {
            const uint32_t address=lines[index].address+uint32_t(consumed);
            if (text.empty()) text_start=address;
            if (address!=text_start+text.size() || text.size()==30) { flush(); text_start=address; }
            const size_t amount=std::min<size_t>(30-text.size(),code.size()-consumed);
            text.insert(text.end(),code.begin()+consumed,code.begin()+consumed+amount);
            consumed+=amount;
            if (text.size()==30) flush();
        }
    }
    flush();
    // Campos simbólicos de formato 4 usam cinco meios-bytes e precisam ser
    // ajustados pelo carregador quando a seção for carregada em outro endereço.
    for (uint32_t address : modifications) object << "M^" << hex(address,6) << "^05+" << header_name << '\n';
    object << "E^" << hex(entry,6) << '\n';

    // A listagem preserva inclusive comentários e linhas vazias para manter a
    // correspondência direta entre o diagnóstico, o fonte e o código gerado.
    std::ostringstream listing;
    for (size_t index=0;index<lines.size();++index) {
        const auto& line=lines[index];
        listing << std::setfill(' ') << std::setw(4) << line.number << "  ";
        if (line.comment) listing << "      "; else listing << hex(line.address,6);
        listing << "  " << std::left << std::setw(10) << bytes_hex(codes[index]) << std::right << "  " << line.original << '\n';
    }
    return {object.str(),listing.str(),symbols};
}
