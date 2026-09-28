#include "sicxe.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
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
    if (text.empty()) return fallback;
    size_t consumed = 0;
    const unsigned long value = std::stoul(text, &consumed, 0);
    if (consumed != text.size() || value > 0xffffffffu) throw std::runtime_error("Número inválido");
    return uint32_t(value);
}

std::map<std::string,std::string> query_parameters(const std::string& target) {
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
    count = std::min(count, 512u);
    if (address >= SicXe::memory_size) throw std::runtime_error("Endereço de visualização fora da memória");
    count = std::min(count, SicXe::memory_size-address);
    std::ostringstream out;
    out << "{\"name\":" << quote(machine.name()) << ",\"loaded\":" << (machine.length()>0 ? "true":"false")
        << ",\"halted\":" << (machine.halted()?"true":"false")
        << ",\"error\":" << quote(machine.error()) << ",\"output\":" << quote(machine.output())
        << ",\"start\":" << machine.start() << ",\"length\":" << machine.length()
        << ",\"steps\":" << machine.steps() << ",\"condition\":" << machine.condition()
        << ",\"inputCount\":" << machine.pending_input() << ",\"floating\":" << machine.floating()
        << ",\"registers\":[";
    const SicXe::Register names[] = {SicXe::A,SicXe::X,SicXe::L,SicXe::B,SicXe::S,SicXe::T,SicXe::PC,SicXe::SW};
    for (size_t i=0; i<8; ++i) { if (i) out << ','; out << machine.reg(names[i]); }
    out << "],\"memoryAddress\":" << address << ",\"memory\":[";
    for (uint32_t i=0; i<count; ++i) { if (i) out << ','; out << unsigned(machine.byte(address+i)); }
    return out.str() + "]}";
}

void send_response(int fd, int code, const std::string& type, const std::string& body) {
    const std::string reason = code==200?"OK":code==400?"Bad Request":code==404?"Not Found":"Internal Server Error";
    std::ostringstream out;
    out << "HTTP/1.1 " << code << ' ' << reason << "\r\nContent-Type: " << type
        << "\r\nContent-Length: " << body.size() << "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n" << body;
    const std::string response = out.str();
    size_t sent = 0;
    while (sent < response.size()) {
        const ssize_t amount = ::send(fd,response.data()+sent,response.size()-sent,0);
        if (amount <= 0) break;
        sent += size_t(amount);
    }
}

void handle(int fd, SicXe& machine) {
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
    std::string body = request.substr(separator+4);
    while (body.size() < content_length) {
        const ssize_t received = ::recv(fd,buffer,sizeof(buffer),0);
        if (received <= 0) throw std::runtime_error("Corpo HTTP incompleto");
        body.append(buffer,size_t(received));
    }
    body.resize(content_length);
    const std::string path = target.substr(0,target.find('?'));
    const auto params = query_parameters(target);
    auto param = [&](const std::string& key) { auto found=params.find(key); return found==params.end()?std::string():found->second; };
    try {
        if (method=="GET" && (path=="/" || path=="/app.js" || path=="/style.css")) {
            const auto web_directory = std::filesystem::read_symlink("/proc/self/exe").parent_path().parent_path() / "web";
            const std::string file = (web_directory / (path=="/"?"index.html":path.substr(1))).string();
            std::ifstream input(file,std::ios::binary);
            if (!input) throw std::runtime_error("Arquivo da interface ausente");
            const std::string contents((std::istreambuf_iterator<char>(input)),std::istreambuf_iterator<char>());
            send_response(fd,200,path=="/"?"text/html; charset=utf-8":path=="/app.js"?"text/javascript; charset=utf-8":"text/css; charset=utf-8",contents);
        } else if (method=="GET" && path=="/api/state") {
            const uint32_t address = number(param("address"),machine.reg(SicXe::PC)&~15u);
            const uint32_t count = number(param("count"),128);
            send_response(fd,200,"application/json; charset=utf-8",state(machine,address,count));
        } else if (method=="POST" && path=="/api/load") {
            machine.load(body,number(param("address")));
            send_response(fd,200,"application/json; charset=utf-8",state(machine,machine.reg(SicXe::PC)&~15u,128));
        } else if (method=="POST" && path=="/api/step") {
            machine.step();
            send_response(fd,200,"application/json; charset=utf-8",state(machine,machine.reg(SicXe::PC)&~15u,128));
        } else if (method=="POST" && path=="/api/run") {
            const uint32_t limit = std::min(number(param("limit"),10000),100000u);
            const int32_t breakpoint = param("breakpoint").empty()?-1:int32_t(number(param("breakpoint")));
            machine.run(limit,breakpoint);
            send_response(fd,200,"application/json; charset=utf-8",state(machine,machine.reg(SicXe::PC)&~15u,128));
        } else if (method=="POST" && path=="/api/reset") {
            machine.reset();
            send_response(fd,200,"application/json; charset=utf-8",state(machine,machine.reg(SicXe::PC)&~15u,128));
        } else if (method=="POST" && path=="/api/input") {
            machine.add_input(body);
            send_response(fd,200,"application/json; charset=utf-8",state(machine,machine.reg(SicXe::PC)&~15u,128));
        } else send_response(fd,404,"application/json; charset=utf-8","{\"error\":\"Rota não encontrada\"}");
    } catch (const std::exception& exception) {
        send_response(fd,400,"application/json; charset=utf-8","{\"error\":"+quote(exception.what())+"}");
    }
}
}

int main(int argc, char** argv) {
    try {
        const uint32_t port = argc>1?number(argv[1]):8080;
        if (port==0 || port>65535) throw std::runtime_error("Porta inválida");
        SicXe machine;
        const int server = ::socket(AF_INET,SOCK_STREAM,0);
        if (server<0) throw std::runtime_error("Não foi possível abrir socket");
        const int reuse = 1;
        ::setsockopt(server,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
        sockaddr_in address{};
        address.sin_family=AF_INET; address.sin_addr.s_addr=htonl(INADDR_LOOPBACK); address.sin_port=htons(uint16_t(port));
        if (::bind(server,reinterpret_cast<sockaddr*>(&address),sizeof(address))<0 || ::listen(server,16)<0)
            throw std::runtime_error("Não foi possível escutar na porta escolhida");
        std::cout << "SIC/XE: http://127.0.0.1:" << port << "\n" << std::flush;
        while (true) {
            const int client = ::accept(server,nullptr,nullptr);
            if (client<0) { if (errno==EINTR) continue; break; }
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
