# Simulador SIC/XE

Simulador em C++17 com interface visual local no navegador, adaptado às instruções em preto do `PRIMEIRO_TRABALHO_SIC_XE.pdf`. C++ foi mantido conforme orientação do responsável pelo projeto. O executor e a interface operam integrados: carga de programas objeto, execução por instrução, execução limitada, ponto de parada, reinício e inspeção de registradores e memória.

## Compilar e executar

Na pasta do projeto, execute:

```sh
./executar.sh
```

O script inicia o servidor e abre o navegador em <http://127.0.0.1:8080>. Se o executável estiver ausente ou os fontes/testes estiverem mais recentes, recompila e testa com `sh ./compilar.sh`. Esse script usa CMake e um compilador C++17; na ausência deles, usa Podman com Alpine para gerar um executável estático, precisando de rede e permissão para contêineres. Para usar outra porta: `./executar.sh 8090`. Mantenha o terminal aberto enquanto utiliza a interface. Abrir `web/index.html` diretamente não inicia o executor.

Para compilar manualmente, em Fedora os pacotes são `gcc-c++`, `cmake`, `make` e, para testes HTTP, `python3`. O diretório separado evita conflitos com caches antigos gerados em contêiner:

```sh
cmake -S . -B build/local
cmake --build build/local -j2
ctest --test-dir build/local --output-on-failure
./build/local/sicxe_sim 8080
```

A porta é opcional; o servidor escuta apenas em `127.0.0.1`. Clique em **Inserir exemplo de soma** e **Carregar programa** para começar. O arquivo [examples/soma.obj](examples/soma.obj) contém o mesmo exemplo; após a execução, `A` e a palavra no endereço `0x0000C` valem 12.

`sh ./compilar.sh` compila, roda os testes e atualiza `build/sicxe_sim` apenas se eles passarem. Python 3 habilita os testes HTTP; os testes do executor não exigem dependências adicionais. Executáveis antigos não validam alterações nos fontes.

Para gerar um pacote Linux com executável, interface, exemplos e documentação, execute `cpack` dentro de `build/local`. Extraia o `.tar.gz` e execute `bin/sicxe_sim`. O pacote é destinado à arquitetura/ambiente de compilação e não é publicado automaticamente.

## Exemplos

- [Soma](examples/soma.obj): resultado 12 em A e na palavra em `0xC`; quatro instruções.
- [Laço](examples/laco.obj): soma de 1 a 5; resultado 15 em A e na palavra em `0x14`, X=5. Use breakpoint em `0x7` para examinar o laço.
- [Sub-rotina](examples/subrotina.obj): preserva L em S, chama e retorna; resultado 12 em A e na palavra em `0x19`.

Os exemplos usam endereçamento relativo ao PC e podem ser carregados em `0x1000`; some esse deslocamento aos endereços de resultados e breakpoints.

## Formato e comportamento

- Aceita registros objeto `H`, `T`, `M` e `E`, com campos fixos ou separados por `^`. A carga em outro endereço aplica relocação aos endereços dos registros e aos campos indicados por `M`.
- A seção de controle é única. Registros `M` podem referenciar seu próprio nome; símbolos externos não são resolvidos.
- Memória de 1 MiB, palavras de 24 bits em três bytes consecutivos (big-endian), registrador `F` de 48 bits. Endereçamento por **byte**, conforme os formatos e operações da tabela; a inconsistência com a expressão “unidade de endereçamento: palavra” está registrada na documentação, sem presumir confirmação do professor.
- Executa as 38 instruções em preto, nos formatos 2, 3 e 4 e modo SIC compatível. As 21 instruções em vermelho são rejeitadas com mensagem explicativa; todos os opcodes de formato 1 da tabela estão nesse grupo. Não há operações de dispositivos nesta etapa.
- `RSUB` no nível principal encerra a execução; `L` começa com o endereço sentinela `0xFFFFF`.
- `F` é exposto para inspeção em hexadecimal e permanece zerado, pois suas instruções estão excluídas.
- Erros preservam os registradores da instrução que falhou e seu PC para inspeção. O carregador não é um montador de assembly.

O servidor é sequencial, usa APIs Linux/POSIX e foi concebido para uso local por uma pessoa. Reiniciar recarrega o programa e reinicializa os registradores.

## Documentação e entrega

- [Documentação técnica](docs/IMPLEMENTACAO.md): estruturas, funções, estratégias, correspondência com o PDF, API e testes.
