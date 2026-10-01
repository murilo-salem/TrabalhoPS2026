#include "sicxe.hpp"

#include <iomanip>
#include <initializer_list>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {
// Usa exceções para que a falha seja visível também em compilações Release.
void check(bool ok, const std::string& label) {
    if (!ok) throw std::runtime_error(label);
}

std::string hex(uint32_t value, int width) {
    std::ostringstream out;
    out << std::hex << std::uppercase << std::setfill('0') << std::setw(width) << value;
    return out.str();
}

// Monta registros objeto pequenos; os resultados esperados são definidos nos casos.
std::string object(const std::string& code, const std::string& data = "") {
    std::string result = "H^TEST^000000^001000\n";
    for (size_t offset=0; offset<code.size(); offset+=60) {
        const auto chunk = code.substr(offset,60);
        result += "T^" + hex(0x100+uint32_t(offset/2),6) + "^" + hex(uint32_t(chunk.size()/2),2) + "^" + chunk + "\n";
    }
    return result + data + "E^000100\n";
}

void run_ok(SicXe& m, const std::string& code, size_t steps, const std::string& data = "") {
    // Cada cenário começa com uma carga limpa para não depender do teste anterior.
    m.load(object(code,data));
    m.run(steps);
    check(m.error().empty(), code + ": " + m.error());
    check(m.steps()==steps, code + ": contagem de instruções");
}

void instruction_set() {
    SicXe m;
    // Aritmética, lógica, sinal e redução para 24 bits.
    const struct { const char* code; uint32_t expected; } arithmetic[] = {
        {"01000C190005",17}, {"01000C1D0005",7},
        {"01000C210005",60}, {"01000C250005",2},
        {"01000C410005",4}, {"01000C450005",13},
        {"0100001D0001",0xffffff}, {"030300190001",0x800000},
        {"030303250002",0xffffff}, {"030303210002",0xfffffa}
    };
    for (const auto& test : arithmetic) {
        run_ok(m,test.code,2,"T^000300^06^7FFFFFFFFFFD\n");
        check(m.reg(SicXe::A)==test.expected,test.code);
    }
    // Cada par carrega um registrador e grava seu conteúdo na mesma palavra.
    const struct { uint8_t load, store; SicXe::Register reg; } registers[] = {
        {0x01,0x0f,SicXe::A}, {0x05,0x13,SicXe::X}, {0x09,0x17,SicXe::L},
        {0x69,0x7b,SicXe::B}, {0x6d,0x7f,SicXe::S}, {0x75,0x87,SicXe::T}
    };
    for (const auto& r : registers) {
        run_ok(m,hex(r.load,2)+"0123"+hex(r.store,2)+"0300",2);
        check(m.reg(r.reg)==0x123 && m.word(0x300)==0x123,"carga/armazenamento de registrador");
    }
    // Instruções de formato 2 codificam r1/r2 nos nibbles do segundo byte.
    const struct { const char* op; uint32_t result; } register_ops[] = {
        {"9001",17}, {"9401",7}, {"9801",60}, {"9C01",2},
        {"AC01",5}, {"B410",0}
    };
    for (const auto& test : register_ops) {
        run_ok(m,std::string("01000505000C")+test.op,3);
        check(m.reg(SicXe::X)==test.result,test.op);
    }
    // Operações de byte não devem modificar os outros bytes da palavra.
    run_ok(m,"030300530303570304",3,"T^000300^06^ABCDEF123456\n");
    check(m.reg(SicXe::A)==0xabcd12 && m.word(0x303)==0x121256,"LDCH/STCH preservam demais bytes");
    run_ok(m,"030300A400",2,"T^000300^03^800001\n");
    check(m.reg(SicXe::A)==3,"SHIFTL circular");
    run_ok(m,"030300A80F",2,"T^000300^03^800001\n");
    check(m.reg(SicXe::A)==0xffff80,"SHIFTR aritmético, 16 bits");
    run_ok(m,"010008A801",2);
    check(m.reg(SicXe::A)==2,"SHIFTR positivo, 2 bits");
    for (int value=4; value<=6; ++value) {
        run_ok(m,"01000"+std::to_string(value)+"290005",2);
        check(m.condition()==value-5,"COMP e CC");
        check(m.reg(SicXe::SW)==uint32_t(value-4),"SW e CC coerentes");
        run_ok(m,"01000"+std::to_string(value)+"050005A001",3);
        check(m.condition()==value-5,"COMPR");
    }
    // TIXR A: incrementa X e compara X com A (registrador 0).
    run_ok(m,"010002B800B800B800",4);
    check(m.reg(SicXe::X)==3 && m.condition()==1,"TIXR");
    run_ok(m,"2D00012D0002",2);
    check(m.reg(SicXe::X)==2 && m.condition()==0,"TIX");
    run_ok(m,"B490",1);
    check(m.condition()==-1 && m.reg(SicXe::SW)==0,"CLEAR SW atualiza CC");
    // Para cada valor de CC, confirma o desvio tomado e o não tomado.
    for (int cc=-1; cc<=1; ++cc) {
        for (const auto op : {0x33,0x37,0x3b}) {
            run_ok(m,"01000"+std::to_string(cc+5)+"290005"+hex(op,2)+"0300",3);
            const bool taken = (op==0x33 && cc==0) || (op==0x37 && cc>0) || (op==0x3b && cc<0);
            check(m.reg(SicXe::PC)==(taken?0x300u:0x109u),"desvio condicional");
        }
    }
    run_ok(m,"3F0300",1);
    check(m.reg(SicXe::PC)==0x300,"J");
    run_ok(m,"4B0300",2,"T^000300^03^4F0000\n");
    check(m.reg(SicXe::L)==0x103 && m.reg(SicXe::PC)==0x103,"JSUB/RSUB");
    run_ok(m,"4F0000",1);
    check(m.halted(),"RSUB principal");
}

void addressing() {
    SicXe m;
    // PC da instrução testada = 0x106; o próximo PC = 0x109 no formato 3.
    const struct { const char* code; uint32_t expected; } modes[] = {
        {"030300",42}, {"03100300",42}, {"0321F7",42}, {"034100",42},
        {"0382FD",42}, {"039002FD",42}, {"03A1F4",42}, {"03C0FD",42},
        {"000300",42}, {"0082FD",42},
        {"020200",42}, {"02100200",42}, {"0220F7",42}, {"024000",42},
        {"010300",0x300}, {"01100300",0x300}, {"0121F7",0x300}, {"014100",0x300},
        {"032F77",77} // Deslocamento negativo relativo ao PC chega em 0x80.
    };
    for (const auto& test : modes) {
        run_ok(m,std::string("050003690200")+test.code,3,
            "T^000080^03^00004D\nT^000200^03^000300\nT^000300^03^00002A\n");
        check(m.reg(SicXe::A)==test.expected,test.code);
    }
    // Em modo SIC, o bit que seria e no XE pertence ao endereço de 15 bits.
    m.load("H^SIC^000000^001003\nT^000100^03^001000\nT^001000^03^00002A\nE^000100\n");
    m.step();
    check(m.error().empty() && m.reg(SicXe::PC)==0x103 && m.reg(SicXe::A)==42,"formato SIC tem 3 bytes");
}

void failures_and_control() {
    SicXe m;
    // Todos os opcodes vermelhos do PDF devem parar antes de modificar o estado.
    for (const auto code : {"58","88","64","C4","C0","F4","70","D0","60","C8","D8",
                            "F0","EC","80","D4","E8","5C","B0","E0","F8","DC"}) {
        m.load(object(code));
        const auto sw = m.reg(SicXe::SW);
        m.step();
        check(m.halted() && m.error().find("excluída")!=std::string::npos,std::string("exclusão ")+code);
        check(m.steps()==0 && m.reg(SicXe::PC)==0x100 && m.reg(SicXe::SW)==sw,"exclusão preserva estado");
    }
    // Decodificação inválida, registradores inexistentes, divisão por zero e limites.
    for (const auto code : {"FF0000","036000","03502000","018001","B470","B460","250000",
                            "9C01","0D0001","550001","031FFFFF","0F1FFFFF","2F1FFFFF"}) {
        m.load(object(code));
        m.step();
        check(m.halted() && !m.error().empty(),std::string("erro esperado: ")+code);
        check(m.reg(SicXe::PC)==0x100 && m.reg(SicXe::X)==0 && m.steps()==0,"erro preserva registradores");
    }
    // Breakpoint interrompe antes da instrução; limite evita laço infinito.
    m.load(object("0100011900013F0103"));
    check(m.run(100,0x103)==1 && m.reg(SicXe::A)==1,"breakpoint antes da instrução");
    check(m.run(5)==5 && !m.halted() && m.reg(SicXe::A)==4,"limite em loop");
    m.reset();
    check(m.reg(SicXe::A)==0 && m.steps()==0 && m.condition()==0 && m.error().empty(),"reset");
    m.load("H^PARTS^000000^000006\nT^000000^06^010005^4F0000\nE^000000\n");
    m.run(3);
    check(m.error().empty() && m.reg(SicXe::A)==5,"T separado em vários campos");
    // Nenhuma carga inválida pode substituir o programa anterior.
    const auto before = m.name();
    for (const auto obj : {
        "H^BAD^000000^000003\nT^000000^03^010001\n",
        "H^BAD^000000^000003\nT^000002^03^010001\nE^000000\n",
        "H^BAD^000000^000003\nE^000000\nT^000000^03^010001\n",
        "H^BAD^000000^000003\nM^000000^05+OTHER\nE^000000\n"}) {
        bool rejected=false;
        try { m.load(obj); } catch (const std::exception&) { rejected=true; }
        check(rejected && m.name()==before && m.reg(SicXe::A)==5,"carga inválida preserva programa");
    }
    // Após uma instrução no fim da memória, a próxima busca informa erro.
    m.load("HEND   000000000002\nT00000002B400\nE000000\n",0xffffe);
    m.step();
    check(m.error().empty() && m.reg(SicXe::PC)==0x100000,"instrução no limite da memória");
    m.step();
    check(m.halted() && !m.error().empty(),"busca fora da memória");
}
}

int main() {
    try {
        instruction_set();
        addressing();
        failures_and_control();
        std::cout << "Requisitos: instruções, endereçamento, exclusões e erros verificados.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
