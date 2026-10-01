"""Testa a API e os arquivos da interface em um servidor recém-compilado."""
import json
from pathlib import Path
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request


def check(value, message):
    if not value:
        raise RuntimeError(message)


# Porta efêmera evita conflito com uma instância aberta pelo usuário.
with socket.socket() as sock:
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]

server = subprocess.Popen([sys.argv[1], str(port)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
base = f"http://127.0.0.1:{port}"


def request(path, data=None, expected=200, raw=False):
    """Envia GET/POST; erros HTTP esperados retornam o corpo para inspeção."""
    if isinstance(data, str):
        data = data.encode()
    try:
        response = urllib.request.urlopen(base + path, data=data, timeout=3)
    except urllib.error.HTTPError as error:
        response = error
    with response:
        check(response.status == expected, f"{path}: HTTP {response.status}, expected {expected}")
        payload = response.read()
    return payload if raw else json.loads(payload)


try:
    # Aguarda a escuta do servidor sem depender de um tempo fixo de inicialização.
    for attempt in range(50):
        try:
            request("/api/state")
            break
        except urllib.error.URLError:
            if server.poll() is not None:
                raise RuntimeError(server.stderr.read().decode())
            time.sleep(0.05)
    else:
        raise RuntimeError("Servidor não iniciou")

    # Verifica a interface e um fluxo real: carregar, pausar, continuar e reiniciar.
    for path in ["/", "/app.js", "/style.css"]:
        check(request(path, raw=True), f"Interface vazia: {path}")
    program = "H^SOMA^000000^00000F\nT^000000^0F^0100051900070F20034F0000000000\nE^000000\n"
    request("/api/load", program)
    check(request("/api/step", "")["registers"][0] == 5, "passo LDA")
    result = request("/api/run?breakpoint=6", "")
    check(result["registers"][6] == 6 and result["steps"] == 2, "breakpoint")
    result = request("/api/run", "")
    check(result["halted"] and not result["error"] and result["registers"][0] == 12, "soma")
    check(request("/api/state?address=00012&count=3")["memory"] == [0, 0, 12], "endereço decimal com zeros")
    request("/api/load", "inválido", expected=400)
    check(request("/api/state")["name"] == "SOMA", "carga atômica")
    result = request("/api/reset", "")
    check(result["steps"] == 0 and not result["halted"], "reset")
    # Os exemplos devem produzir o mesmo resultado após relocação para 0x1000.
    for filename, address, expected, steps in [("laco.obj", 0x14, 15, 20), ("subrotina.obj", 0x19, 12, 8)]:
        program = (Path(__file__).resolve().parent.parent / "examples" / filename).read_text()
        for origin in (0, 0x1000):
            request(f"/api/load?address={origin}", program)
            result = request("/api/run?limit=100", "")
            check(result["halted"] and not result["error"] and result["steps"] == steps, filename)
            check(result["registers"][0] == expected, filename + ": acumulador")
            check(request(f"/api/state?address={origin+address}&count=3")["memory"] == [0, 0, expected], filename + ": resultado")
    # Entradas inválidas são recusadas pela API, independentemente da interface.
    for path in ["/api/run?limit=0", "/api/run?limit=-1", "/api/run?limit=100001", "/api/run?breakpoint=0x100000"]:
        request(path, "", expected=400)
    request("/api/state?address=0x100000", expected=400)
    request("/api/input", "", expected=404)
    request("/api/load", "H^RED^000000^000003\nT^000000^03^DB0000\nE^000000\n")
    result = request("/api/step", "")
    check("excluída" in result["error"] and result["registers"][6] == 0, "instrução vermelha")
    # Mesmo com PC fora da memória, a API precisa devolver o estado para a UI.
    request("/api/load?address=0xFFFFE", "H^END^000000^000002\nT^000000^02^B400\nE^000000\n")
    result = request("/api/step", "")
    check(result["registers"][6] == 0x100000, "PC após última instrução")
    result = request("/api/step", "")
    check(result["halted"] and result["error"], "erro de memória exposto pela API")
    print("HTTP: interface, execução, breakpoint, reset, validações e erros OK.")
finally:
    # Encerra o servidor temporário mesmo se alguma verificação falhar.
    server.terminate()
    try:
        server.wait(timeout=3)
    except subprocess.TimeoutExpired:
        server.kill()
        server.wait()
