#include "assembler.hpp"
#include "sicxe.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void expect_error(const Assembler& assembler, const std::string& source, const std::string& part) {
    try { assembler.assemble(source); }
    catch (const std::exception& error) {
        check(std::string(error.what()).find(part)!=std::string::npos,"diagnóstico inesperado: " + std::string(error.what()));
        return;
    }
    throw std::runtime_error("erro esperado: " + part);
}
}

int main() {
    try {
        Assembler assembler;
        SicXe machine;
        const std::string sum =
            "SOMA START 0\n"
            "      LDA #5\n"
            "      ADD #7\n"
            "      STA RESULT\n"
            "      RSUB\n"
            "RESULT RESW 1\n"
            "      END SOMA\n";
        const auto result=assembler.assemble(sum);
        check(result.symbols.at("RESULT")==12,"SYMTAB da passagem 1");
        check(result.object=="H^SOMA^000000^00000F\nT^000000^0C^0100051900070F20034F0000\nE^000000\n","objeto da soma");
        machine.load(result.object);
        machine.run(20);
        check(machine.halted() && machine.error().empty() && machine.word(12)==12,"objeto montado executável");

        const auto formats=assembler.assemble(
            "MODES START 1000\n"
            "FIRST CLEAR X\n"
            "      SHIFTL A,16\n"
            "      +LDA #74565\n"
            "      +JSUB TARGET\n"
            "      BASE TARGET\n"
            "      LDA TARGET\n"
            "      LDX #-1\n"
            "      LDCH TARGET,X\n"
            "      NOBASE\n"
            "TARGET BYTE C'OK'\n"
            "HEX   BYTE X'F1A0'\n"
            "NUM   WORD -1\n"
            "      END FIRST\n");
        check(formats.object.find("M^001009^05+MODES")!=std::string::npos,"registro de modificação");
        check(formats.object.find("B410A40F01112345")!=std::string::npos,"formatos 2 e 4");
        check(formats.object.find("050FFF53")!=std::string::npos,"imediato negativo e indexação");
        check(formats.object.find("4F4BF1A0FFFFFF")!=std::string::npos,"BYTE e WORD");

        expect_error(assembler,"P START 0\nX WORD 1\nX RESW 1\n END P\n","símbolo duplicado");
        expect_error(assembler,"P START 0\n LDA DISTANTE\n RESB 5000\nDISTANTE WORD 1\n END P\n","fora do alcance");
        expect_error(assembler,"P START 0\n BYTE X'123'\n END P\n","quantidade par");
        expect_error(assembler,"P START 0\n CLEAR Q\n END P\n","registrador inválido");
        std::cout << "Montador: duas passagens, formatos, diretivas e diagnósticos OK.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
