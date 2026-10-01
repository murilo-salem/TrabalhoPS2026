# Documentação técnica

## Requisitos e decisões

A referência é o PDF `PRIMEIRO_TRABALHO_SIC_XE.pdf`, páginas 2–7. O executor usa C++17 por orientação do responsável pelo projeto, embora o PDF mencione Java. A interface usa HTML/CSS/JavaScript; o executor serve seus arquivos e a API no mesmo processo.

| Requisito | Implementação / interpretação |
|---|---|
| Memória não menor que 1 KB | 1 MiB, inicialmente zerado, sem cache ou paridade |
| Palavra de 24 bits | Três bytes consecutivos em ordem big-endian |
| Unidade de endereçamento | Byte, coerente com `m..m+2`, `LDCH`/`STCH` e formatos de instrução |
| Registradores | A/X/L/B/S/T/PC/SW: 24 bits; F: 48 bits, zerado nesta etapa |
| Direto, indireto e imediato | PC relativo com sinal, base relativo sem sinal, indexação válida, formato estendido e compatibilidade SIC |
| Instruções vermelhas | Rejeitadas antes de executar, com mensagem identificando o mnemônico |
| Interface gráfica integrada | Controle do executor, memória, registradores, PC, CC, passos e erros |

O PDF contém ambiguidades: chama a unidade de endereçamento de “palavra”, mas operações e formatos usam bytes; menciona sete registradores, mas enumera nove; mostra `CLEAR` com opcode `4` (já usado por `LDX`); o desenho do formato 2 não separa opcode de registradores. Adotamos os nove registradores enumerados, `CLEAR = B4` e formato 2 com opcode de 8 bits seguido por dois campos de 4 bits. Essas decisões mantêm os programas objeto coerentes com SIC/XE e devem ser explicadas na apresentação. Uma exigência literal de endereçamento por palavra requer esclarecimento do professor e revisão do modelo, não apenas da visualização.

## Estruturas de dados e módulos

`SicXe` (`src/sicxe.hpp` e `src/sicxe.cpp`) encapsula o estado e não depende da interface:

| Estrutura | Função |
|---|---|
| `vector<uint8_t> memory_` | 1.048.576 bytes; endereços de 0 a `0xFFFFF` |
| `array<uint32_t,10> registers_` | Slots com a numeração da tabela; valores limitados a 24 bits; slots 6 e 7 não são registradores inteiros |
| `uint64_t floating_` | F, exposto com 12 dígitos hexadecimais; permanece zero |
| `condition_`, `halted_`, `error_`, `steps_` | Condição, parada, diagnóstico e instruções concluídas |
| `object_`, `name_`, endereços e comprimento | Programa original e metadados, também usados pelo reinício |
| `Record { type, fields }` | Registro objeto intermediário para validar carga e aplicar relocação |

O servidor em `src/main.cpp` valida parâmetros e serializa JSON. A interface liga os controles às rotas e usa o estado retornado pelo executor. Destaca alterações dos registradores e o byte apontado pelo PC. Com PC fora da memória, a inspeção permanece na última linha válida para manter o diagnóstico visível.

## Funções e estratégias

| Função | Responsabilidade |
|---|---|
| `load(object,address)` | Valida H/T/M/E, monta memória temporária e substitui o programa apenas após validação completa |
| `reset()` | Repete a carga do objeto original no endereço anterior |
| `step()` | Busca, decodifica, calcula operando, executa e conta uma instrução; em erro restaura registradores e registra a parada |
| `run(limit,breakpoint)` | Repete `step`, parando antes do breakpoint, no limite ou em encerramento/erro |
| `byte`, `word`, `put_byte`, `put_word` | Leitura/escrita com verificação de limites; palavra em três bytes |
| `signed24`, `mask24` | Complemento de dois e redução módulo 2²⁴ |
| `compare` | Atualiza CC e dois bits baixos de SW, preservando os demais |
| `state` no servidor | Produz JSON com metadados, registradores e janela de memória |
| `render` / `renderMemory` na interface | Atualizam os painéis e a tabela de memória |

O cálculo de endereço usa o PC da próxima instrução. Deslocamento relativo ao PC tem 12 bits com sinal; base relativo tem 12 bits sem sinal. Formato 4 usa endereço de 20 bits. Em modo SIC (`n=i=0`), o endereço ocupa 15 bits e a instrução sempre tem três bytes. A indexação soma X; no indireto, a palavra no endereço calculado fornece o endereço final; no imediato, o endereço calculado é o próprio valor.

Aritmética inteira usa complemento de dois e descarta bits acima de 24; divisão trunca em direção a zero e rejeita divisor zero. `SHIFTL` é circular à esquerda; `SHIFTR` replica o bit de sinal; a contagem codificada contém `n-1` (1 a 16 deslocamentos). `LDCH` preserva os 16 bits superiores de A; `STCH` escreve somente o byte inferior.

CC usa -1, 0 e 1 na API; nos dois bits baixos de SW, 0 significa `<`, 1 significa `=` e 2 significa `>`. Inicialmente SW=1 e CC=`=`. Operações inteiras com destino SW também atualizam CC; a combinação 3 é tratada como `=`. F não é operando das operações inteiras de formato 2.

O PC avança pelo comprimento da instrução, salvo desvios e retornos. `JSUB` salva o próximo PC em L. `RSUB` transfere L para PC. L é inicialmente `0xFFFFF`; atingir esse valor encerra a execução. Essa convenção reserva esse endereço como sentinela de execução. Chamadas aninhadas exigem salvar/restaurar L. Erros não incrementam `steps`; `run` retorna tentativas de passo, inclusive a tentativa que falhou.

## Conjunto de instruções

As 38 instruções em preto possuem execução:

| Grupo | Instruções |
|---|---|
| Aritmética e lógica | ADD, ADDR, SUB, SUBR, MUL, MULR, DIV, DIVR, AND, OR |
| Carga e armazenamento | LDA, LDX, LDL, LDB, LDS, LDT, LDCH, STA, STX, STL, STB, STS, STT, STCH |
| Registradores | CLEAR, RMO, SHIFTL, SHIFTR |
| Comparação e laços | COMP, COMPR, TIX, TIXR |
| Controle | J, JEQ, JGT, JLT, JSUB, RSUB |

As 21 instruções vermelhas são reconhecidas apenas para diagnóstico, sem executar efeitos: ADDF, COMPF, DIVF, FIX, FLOAT, HIO, LDF, LPS, MULF, NORM, RD, SIO, SSK, STF, STI, STSW, SUBF, SVC, TD, TIO, WD. Não há dispositivos, interrupções, temporizador ou proteção por chaves. Todos os formatos 1 da tabela são excluídos nesta etapa.

## Carga e interface HTTP

O carregador aceita uma seção: H define nome, origem e comprimento; T fornece até 30 bytes por registro; M modifica até seis dígitos hexadecimais; E define a entrada. Campos T podem estar juntos ou separados por `^`; registros fixos também são aceitos. A memória não preenchida permanece zero. Relocação aplica `endereço de carga - origem`; M com cinco dígitos preserva o nibble superior. Símbolos externos e múltiplas seções não são resolvidos. Registros inválidos não substituem o programa carregado.

| Método e rota | Entrada / efeito |
|---|---|
| GET `/api/state?address=0&count=128` | Consulta até 512 bytes; endereço padrão segue PC |
| POST `/api/load?address=0` | Corpo em texto com programa objeto; carrega e reinicializa |
| POST `/api/step` | Executa uma instrução |
| POST `/api/run?limit=10000&breakpoint=12` | De 1 a 100.000 tentativas; breakpoint opcional |
| POST `/api/reset` | Recarrega o programa original |

Respostas de sucesso contêm `name`, `loaded`, `halted`, `error`, `start`, `length`, `steps`, `condition`, `floating`, `registers`, `memoryAddress`, `memory`. Ordem de `registers`: A, X, L, B, S, T, PC, SW; F é `floating`. Endereços aceitam decimal (inclusive com zeros iniciais) e hexadecimal com `0x`. Erros de requisição retornam HTTP 400 e `error`; erros da máquina retornam HTTP 200 com estado `halted` e diagnóstico, permitindo inspeção. A antiga rota `/api/input` e os campos `inputCount`/`output` foram removidos junto às funções excluídas.

## Verificação e limitações

`sh compilar.sh` recompila e executa CTest. Os testes originais cobrem soma, reinício, relocação, formato estendido, indireto e carga inválida atômica. `sicxe_requirements_tests` cobre representantes das famílias de instruções exigidas, combinações de endereçamento da tabela, deslocamento negativo, compatibilidade SIC, sinais, overflow, bytes, desvios tomados/não tomados, sub-rotina, limites, exclusões, registradores/flags inválidos e divisão por zero. As verificações permanecem ativas em Release; ainda não há um caso isolado para cada uma das 38 instruções.

Com Python 3, `sicxe_http_tests` inicia servidor temporário e verifica arquivos da interface, carga, passo, breakpoint, soma, reinício, parâmetros inválidos, exclusões e PC fora da memória. Não substitui inspeção visual no navegador. Testes não são prova formal de correção. O servidor usa APIs Linux/POSIX.

A entrega requer testes sobre compilação atualizada, conferência visual, disponibilização no GitHub e gravação da apresentação pelo grupo.
