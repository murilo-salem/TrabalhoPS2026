#include "sicxe.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {
// Serializa uma string JSON. Nome de programa e erro podem vir do arquivo
// enviado pelo usuário; aspas, barras e caracteres de controle precisam ser
// escapados antes de entrar na resposta da API.
std::string quote(const std::string& value) {
    std::ostringstream output;
    output << '"';
    for (unsigned char ch : value) {
        switch (ch) {
            case '"': output << "\\\""; break;
            case '\\': output << "\\\\"; break;
            case '\n': output << "\\n"; break;
            case '\r': output << "\\r"; break;
            case '\t': output << "\\t"; break;
            default:
                if (ch < 32) output << "\\u" << std::hex << std::setw(4) << std::setfill('0') << int(ch) << std::dec;
                else output << ch;
        }
    }
    return output.str() + '"';
}

uint32_t number(const std::string& text, uint32_t fallback = 0) {
    // Os parâmetros da URL são decimais, salvo prefixo explícito 0x. O valor
    // padrão é usado quando o parâmetro foi omitido ou veio vazio.
    if (text.empty()) return fallback;
    if (text.front() == '-' || text.front() == '+' || std::isspace(static_cast<unsigned char>(text.front())))
        throw std::runtime_error("Número inválido");
    size_t consumed = 0;
    const bool hexadecimal = text.size()>2 && text[0]=='0' && (text[1]=='x' || text[1]=='X');
    const unsigned long value = std::stoul(text, &consumed, hexadecimal ? 16 : 10);
    // Exige consumo completo: "12abc" não pode ser aceito como endereço 12.
    if (consumed != text.size() || value > 0xffffffffu) throw std::runtime_error("Número inválido");
    return uint32_t(value);
}

std::map<std::string,std::string> query_parameters(const std::string& target) {
    // Separa a parte depois de '?' em pares chave=valor. As rotas atuais usam
    // parâmetros numéricos simples enviados pelo próprio app.js.
    std::map<std::string,std::string> result;
    const auto mark = target.find('?');
    if (mark == std::string::npos) return result;
    std::stringstream stream(target.substr(mark+1));
    std::string item;
    while (std::getline(stream,item,'&')) {
        const auto equal = item.find('=');
        if (equal != std::string::npos) result[item.substr(0,equal)] = item.substr(equal+1);
    }
    return result;
}

std::string state(const SicXe& machine, uint32_t address, uint32_t count) {
    // Monta o JSON consumido por render() no navegador. A memória inteira tem
    // 1 MiB; a resposta inclui só uma janela de até 512 bytes para cada ação.
    count = std::min(count, 512u);
    if (address >= SicXe::memory_size) throw std::runtime_error("Endereço de visualização fora da memória");
    count = std::min(count, SicXe::memory_size-address);
    std::ostringstream out;
    out << "{\"name\":" << quote(machine.name()) << ",\"loaded\":" << (machine.length()>0 ? "true":"false")
        << ",\"halted\":" << (machine.halted()?"true":"false")
        << ",\"error\":" << quote(machine.error())
        << ",\"start\":" << machine.start() << ",\"length\":" << machine.length()
        << ",\"steps\":" << machine.steps() << ",\"condition\":" << machine.condition()
        << ",\"floating\":" << machine.floating()
        << ",\"registers\":[";
    // A ordem deste vetor é o contrato com web/app.js: A, X, L, B, S, T,
    // PC, SW. F é enviado separadamente em "floating" por ter 48 bits.
    const SicXe::Register names[] = {SicXe::A,SicXe::X,SicXe::L,SicXe::B,SicXe::S,SicXe::T,SicXe::PC,SicXe::SW};
    for (size_t i=0; i<8; ++i) { if (i) out << ','; out << machine.reg(names[i]); }
    out << "],\"memoryAddress\":" << address << ",\"memory\":[";
    for (uint32_t i=0; i<count; ++i) { if (i) out << ','; out << unsigned(machine.byte(address+i)); }
    return out.str() + "]}";
}

uint32_t memory_view(const SicXe& machine) {
    // Alinha ao início de uma linha de 16 bytes da tabela. Caso PC ultrapasse
    // o espaço válido, a API ainda devolve a última linha para mostrar o erro.
    return std::min(machine.reg(SicXe::PC), SicXe::memory_size-1) & ~15u;
}

void send_response(int fd, int code, const std::string& type, const std::string& body) {
    // Cada chamada recebe uma resposta HTTP/1.1 com Content-Length e fecha a
    // conexão. O navegador usa no-store para não mostrar estado antigo.
    const std::string reason = code==200?"OK":code==400?"Bad Request":code==404?"Not Found":"Internal Server Error";
    std::ostringstream out;
    out << "HTTP/1.1 " << code << ' ' << reason << "\r\nContent-Type: " << type
        << "\r\nContent-Length: " << body.size() << "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n" << body;
    const std::string response = out.str();
    size_t sent = 0;
    // send pode escrever menos bytes do que foi pedido. Repetimos até enviar
    // tudo ou detectar desconexão; MSG_NOSIGNAL evita encerrar o processo.
    while (sent < response.size()) {
        const ssize_t amount = ::send(fd,response.data()+sent,response.size()-sent,MSG_NOSIGNAL);
        if (amount <= 0) break;
        sent += size_t(amount);
    }
}

void handle(int fd, SicXe& machine) {
    // Lê até o separador CRLF CRLF. recv pode dividir uma mesma requisição em
    // vários pacotes; por isso não supomos que a primeira leitura é completa.
    std::string request;
    char buffer[4096];
    while (request.find("\r\n\r\n") == std::string::npos) {
        const ssize_t received = ::recv(fd,buffer,sizeof(buffer),0);
        if (received <= 0) return;
        request.append(buffer,size_t(received));
        if (request.size() > 65536) throw std::runtime_error("Cabeçalho HTTP muito grande");
    }
    const size_t separator = request.find("\r\n\r\n");
    const std::string headers = request.substr(0,separator);
    // A linha inicial define método/caminho. Procuramos Content-Length sem
    // depender de maiúsculas/minúsculas no nome do cabeçalho.
    std::istringstream first(headers);
    std::string method,target,version;
    first >> method >> target >> version;
    size_t content_length = 0;
    std::istringstream header_stream(headers);
    std::string line;
    while (std::getline(header_stream,line)) {
        if (!line.empty() && line.back()=='\r') line.pop_back();
        const auto colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0,colon);
            std::transform(key.begin(),key.end(),key.begin(),[](unsigned char c){return std::tolower(c);});
            if (key=="content-length") content_length = std::stoul(line.substr(colon+1));
        }
    }
    if (content_length > 2*1024*1024) throw std::runtime_error("Corpo HTTP muito grande");
    // Parte do corpo pode ter vindo junto dos cabeçalhos. Lê apenas o restante
    // indicado por Content-Length e descarta bytes excedentes deste pedido.
    std::string body = request.substr(separator+4);
    while (body.size() < content_length) {
        const ssize_t received = ::recv(fd,buffer,sizeof(buffer),0);
        if (received <= 0) throw std::runtime_error("Corpo HTTP incompleto");
        body.append(buffer,size_t(received));
    }
    body.resize(content_length);
    // O caminho seleciona a rota; a consulta fica em params. param devolve
    // string vazia quando a chave não existe, permitindo aplicar padrões.
    const std::string path = target.substr(0,target.find('?'));
    const auto params = query_parameters(target);
    auto param = [&](const std::string& key) { auto found=params.find(key); return found==params.end()?std::string():found->second; };
    try {
        // Arquivos estáticos e API compartilham porta e origem. Assim app.js
        // usa fetch('/api/...') sem configurar outro servidor ou CORS.
        if (method=="GET" && (path=="/" || path=="/app.js" || path=="/style.css")) {
            // Na compilação local, web/ fica junto do binário; no pacote gerado
            // por CPack, o executável está em bin/ e web/ fica um nível acima.
            const auto executable_directory = std::filesystem::read_symlink("/proc/self/exe").parent_path();
            const auto adjacent_web = executable_directory / "web";
            const auto web_directory = std::filesystem::exists(adjacent_web / "index.html") ? adjacent_web : executable_directory.parent_path() / "web";
            const std::string file = (web_directory / (path=="/"?"index.html":path.substr(1))).string();
            std::ifstream input(file,std::ios::binary);
            if (!input) throw std::runtime_error("Arquivo da interface ausente");
            const std::string contents((std::istreambuf_iterator<char>(input)),std::istreambuf_iterator<char>());
            send_response(fd,200,path=="/"?"text/html; charset=utf-8":path=="/app.js"?"text/javascript; charset=utf-8":"text/css; charset=utf-8",contents);
        } else if (method=="GET" && path=="/api/state") {
            // Consulta não altera a máquina. Se endereço faltar, segue o PC;
            // count controla quantos bytes entram na janela de memória.
            const uint32_t address = number(param("address"),memory_view(machine));
            const uint32_t count = number(param("count"),128);
            send_response(fd,200,"application/json; charset=utf-8",state(machine,address,count));
        } else if (method=="POST" && path=="/api/load") {
            // O corpo é o texto dos registros objeto; address é a posição de
            // carga escolhida na interface. Erro de carga preserva o estado.
            machine.load(body,number(param("address")));
            send_response(fd,200,"application/json; charset=utf-8",state(machine,memory_view(machine),128));
        } else if (method=="POST" && path=="/api/step") {
            // Executa uma tentativa de instrução e devolve o estado resultante.
            machine.step();
            send_response(fd,200,"application/json; charset=utf-8",state(machine,memory_view(machine),128));
        } else if (method=="POST" && path=="/api/run") {
            // A API repete a validação feita no navegador: outras chamadas
            // HTTP também precisam obedecer ao limite e ao espaço de memória.
            const uint32_t limit = number(param("limit"),10000);
            if (limit < 1 || limit > 100000) throw std::runtime_error("Limite deve estar entre 1 e 100000");
            const uint32_t stop = number(param("breakpoint"));
            if (stop >= SicXe::memory_size) throw std::runtime_error("Ponto de parada fora da memória");
            const int32_t breakpoint = param("breakpoint").empty()?-1:int32_t(stop);
            machine.run(limit,breakpoint);
            send_response(fd,200,"application/json; charset=utf-8",state(machine,memory_view(machine),128));
        } else if (method=="POST" && path=="/api/reset") {
            // Restaura o programa carregado e seu endereço de carga anterior.
            machine.reset();
            send_response(fd,200,"application/json; charset=utf-8",state(machine,memory_view(machine),128));
        } else send_response(fd,404,"application/json; charset=utf-8","{\"error\":\"Rota não encontrada\"}");
    } catch (const std::exception& exception) {
        // Problemas de parâmetro, carga ou leitura viram HTTP 400 com JSON;
        // erros de instrução ficam no estado da máquina retornado com HTTP 200.
        send_response(fd,400,"application/json; charset=utf-8","{\"error\":"+quote(exception.what())+"}");
    }
}
}

int main(int argc, char** argv) {
    try {
        // Por padrão usa 8080. O socket aceita conexões apenas no loopback,
        // pois há uma única máquina compartilhada por todas as requisições.
        const uint32_t port = argc>1?number(argv[1]):8080;
        if (port==0 || port>65535) throw std::runtime_error("Porta inválida");
        SicXe machine;
        const int server = ::socket(AF_INET,SOCK_STREAM,0);
        if (server<0) throw std::runtime_error("Não foi possível abrir socket");
        const int reuse = 1;
        // Permite reiniciar o servidor logo após encerrá-lo sem esperar a porta.
        ::setsockopt(server,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
        sockaddr_in address{};
        address.sin_family=AF_INET; address.sin_addr.s_addr=htonl(INADDR_LOOPBACK); address.sin_port=htons(uint16_t(port));
        if (::bind(server,reinterpret_cast<sockaddr*>(&address),sizeof(address))<0 || ::listen(server,16)<0)
            throw std::runtime_error("Não foi possível escutar na porta escolhida");
        std::cout << "SIC/XE: http://127.0.0.1:" << port << "\n" << std::flush;
        // Atende uma conexão por vez. Uma ação termina antes de a próxima
        // começar, portanto renderização e execução veem estados consistentes.
        while (true) {
            const int client = ::accept(server,nullptr,nullptr);
            if (client<0) { if (errno==EINTR) continue; break; }
            // Fecha o descritor mesmo se handle detectar requisição inválida.
            try { handle(client,machine); }
            catch (const std::exception& exception) { send_response(client,400,"application/json; charset=utf-8","{\"error\":"+quote(exception.what())+"}"); }
            ::close(client);
        }
        ::close(server);
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
