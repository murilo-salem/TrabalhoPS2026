# Simulador SIC/XE

Simulador em C++17 com interface visual local no navegador. O executor e a interface são servidos pelo mesmo processo. A interface permite carregar programas objeto, avançar instruções, executar com limite e ponto de parada, reiniciar, fornecer bytes de entrada e observar registradores, memória e saída.

## Compilar e executar

Na pasta do projeto, execute:

```sh
./executar.sh
```

O script inicia o servidor e abre o navegador em <http://127.0.0.1:8080>. Caso o executável ainda não exista, usa CMake e um compilador C++17; na ausência deles, usa Podman para gerar um executável estático. Para usar outra porta: `./executar.sh 8090`. Mantenha o terminal aberto enquanto utiliza a interface. Abrir `web/index.html` diretamente não inicia o executor.

Para compilar manualmente, em Fedora os pacotes necessários são `gcc-c++` e `cmake`:

```sh
cmake -S . -B build
cmake --build build
./build/sicxe_sim 8080
```

A porta é opcional; o servidor escuta apenas em `127.0.0.1`. Clique em **Inserir exemplo de soma** e **Carregar programa** para começar. O arquivo [examples/soma.obj](examples/soma.obj) contém o mesmo exemplo; após a execução, `A` e a palavra no endereço `0x0000C` valem 12.

Para rodar os testes: `ctest --test-dir build --output-on-failure`.

## Formato e comportamento

- Aceita registros objeto `H`, `T`, `M` e `E`, com campos fixos ou separados por `^`. A carga em outro endereço aplica relocação aos endereços dos registros e aos campos indicados por `M`.
- A seção de controle é única. Registros `M` podem referenciar seu próprio nome; símbolos externos não são resolvidos.
- Memória de 1 MiB, palavras de 24 bits, registrador `F` de 48 bits, instruções dos formatos 1 a 4 e modo SIC compatível.
- `RD` consome um byte da fila de entrada; `WD` acrescenta o byte baixo de `A` à saída; `TD` indica disponibilidade da fila (`<` quando há dados, `=` quando vazia). Os números de dispositivo do operando são ignorados. Um `RD` sem entrada interrompe a execução com erro.
- `RSUB` no nível principal encerra a execução; `L` começa com o endereço sentinela `0xFFFFF`.
- Instruções de canal e privilégios (`HIO`, `SIO`, `SVC`, `LPS`, `STI`, `SSK`) interrompem a execução com uma mensagem de recurso não suportado.
- `F` é exposto para inspeção em hexadecimal; seu formato segue sinal de 1 bit, expoente de 11 bits com viés 1024 e fração de 36 bits.

O servidor é sequencial e foi concebido para uso local por uma pessoa. Reiniciar recarrega o último programa e limpa entrada, saída e estado dos registradores.
