#include "sicxe.hpp"

// O assert deve continuar ativo mesmo se o projeto for compilado em modo Release.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <iostream>
#include <stdexcept>

int main() {
    SicXe machine;
    // Soma básica: valida carga, quatro instruções, resultado e convenção de RSUB.
    const std::string sum = "H^SOMA^000000^00000F\nT^000000^0F^0100051900070F20034F0000000000\nE^000000\n";
    machine.load(sum);
    assert(machine.reg(SicXe::PC) == 0);
    assert(machine.run(20) == 4);
    assert(machine.halted() && machine.error().empty());
    assert(machine.word(12) == 12);
    assert(machine.reg(SicXe::A) == 12);
    machine.reset();
    assert(!machine.halted() && machine.word(12) == 0);
    machine.load(sum,0x1000);
    // O mesmo objeto pode ser carregado em outro endereço sem mudar o resultado.
    assert(machine.reg(SicXe::PC)==0x1000);
    machine.run(20);
    assert(machine.word(0x100c)==12);

    // Operação entre registradores (formato 2) e desvio relativo ao PC.
    machine.load("H^MODES^000000^000013\n"
                 "T^000000^13^010003B41090012900033320030100004F0000\n"
                 "E^000000\n");
    machine.step();
    assert(machine.reg(SicXe::A)==3);
    machine.step();
    assert(machine.reg(SicXe::X)==0);
    machine.step();
    assert(machine.reg(SicXe::X)==3);
    machine.run(20);
    assert(machine.halted() && machine.error().empty());
    assert(machine.reg(SicXe::A)==3);

    machine.load("H^INDIR^000000^00000F\n"
                 "T^000000^0F^0220064F000000000000000C00002A\n"
                 "E^000000\n");
    machine.step();
    assert(machine.reg(SicXe::A)==42);

    machine.load("H^EXTEND^000000^000007\nT^000000^07^011123454F0000\nE^000000\n");
    // Formato 4 usa o campo de endereço de 20 bits.
    machine.run(10);
    assert(machine.reg(SicXe::A)==0x12345 && machine.halted());

    // M de cinco dígitos altera só os 20 bits inferiores do campo.
    machine.load("H^RELOC^000000^000007\nT^000000^07^031000004F0000\nM^000001^05+RELOC\nE^000000\n",0x300);
    assert(machine.byte(0x301)==0x10);
    assert(machine.byte(0x302)==0x03 && machine.byte(0x303)==0x00);

    bool failed=false;
    try { machine.load("H^BAD^000000^000003\nT^000000^03^GG0000\nE^000000\n"); }
    catch (const std::runtime_error&) { failed=true; }
    assert(failed);
    assert(machine.name()=="RELOC"); // Carga inválida preserva o programa anterior.

    std::cout << "Todos os testes passaram.\n";
}
