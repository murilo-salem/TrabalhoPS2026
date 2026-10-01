#pragma once

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

// Representa uma única máquina SIC/XE: memória, registradores, programa carregado
// e estado de execução. O servidor HTTP apenas chama esta API; a classe pode ser
// usada diretamente pelos testes sem navegador ou socket.
class SicXe {
public:
    // Há 1 MiB de endereços de byte (0x00000..0xFFFFF). Uma palavra de 24 bits
    // começa no endereço informado e ocupa esse byte e os dois seguintes.
    static constexpr uint32_t memory_size = 1u << 20;
    static constexpr uint32_t word_mask = 0xffffff;
    // Os números seguem a tabela do SIC/XE. F é o registrador 6 de 48 bits;
    // por não caber nos slots de 24 bits, seu valor fica em floating_.
    enum Register { A=0, X=1, L=2, B=3, S=4, T=5, F=6, PC=8, SW=9 };

    SicXe();
    // Recebe texto com registros objeto H/T/M/E e um endereço de carga opcional.
    // Valida e reloca em memória temporária; erro deixa o programa anterior intacto.
    void load(const std::string& object, uint32_t address = 0);
    // Recarrega o texto original no último endereço escolhido. Isso reinicia
    // memória, registradores, contador de passos e diagnóstico.
    void reset();
    // step tenta uma instrução: no sucesso atualiza PC e steps(); no erro para,
    // guarda a mensagem em error() e restaura os registradores da tentativa.
    void step();
    // run executa até parada, limite ou breakpoint (testado antes da instrução).
    // O retorno conta tentativas de passo, inclusive uma que terminou em erro.
    size_t run(size_t limit = 10000, int32_t breakpoint = -1);
    // Leitura de um byte ou de três bytes em ordem big-endian. Acesso inválido
    // lança exceção, inclusive quando uma palavra cruza o fim da memória.
    uint8_t byte(uint32_t address) const;
    uint32_t word(uint32_t address) const;
    uint32_t reg(Register r) const { return registers_[r]; }
    uint64_t floating() const { return floating_; }
    int condition() const { return condition_; }
    bool halted() const { return halted_; }
    const std::string& error() const { return error_; }
    const std::string& name() const { return name_; }
    uint32_t start() const { return start_; }
    uint32_t length() const { return length_; }
    uint64_t steps() const { return steps_; }

private:
    // memory_ usa byte como unidade. registers_ usa os números do enunciado:
    // 0..5 são A/X/L/B/S/T, 8 é PC, 9 é SW. O slot 6 pertence a F, guardado
    // separadamente; o slot 7 não corresponde a um registrador.
    std::vector<uint8_t> memory_;
    std::array<uint32_t, 10> registers_{};
    // F permanece zero nesta etapa, porque todas as suas instruções estão excluídas.
    uint64_t floating_ = 0;
    // condition_: -1 (<), 0 (=), 1 (>). Os dois bits baixos de SW codificam
    // a mesma condição como 0, 1 e 2; compare mantém os dois valores coerentes.
    int condition_ = 0;
    // halted_ impede novos passos; error_ diferencia erro de término normal.
    bool halted_ = true;
    // object_ guarda o texto original para reset(), sem depender de um arquivo
    // ainda aberto na interface. name_ vem do registro H.
    std::string error_, name_, object_;
    // start_/length_ descrevem a seção carregada; entry_ vem de E; load_address_
    // guarda a escolha do usuário para recarregar o programa no mesmo lugar.
    uint32_t start_ = 0, length_ = 0, entry_ = 0, load_address_ = 0;
    // Conta somente instruções concluídas; erros não incrementam o contador.
    uint64_t steps_ = 0;

    // Versões de escrita internas para as instruções de armazenamento.
    void put_byte(uint32_t address, uint8_t value);
    void put_word(uint32_t address, uint32_t value);
    // Interpreta o bit de sinal de uma palavra e limita resultados a 24 bits.
    static int32_t signed24(uint32_t value);
    static uint32_t mask24(int64_t value);
    // Atualiza CC sem apagar outros bits da palavra de status SW.
    void compare(int64_t left, int64_t right);
};
