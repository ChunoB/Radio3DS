"""Grava a versão certa (major.minor.micro) dentro de um .cia.

O makerom 0.19 ignora o -minor e grava a versão do título como major.0.micro.
Assim a 1.10.0 virava "1.0.0", mais velha que a 1.9.1 ("1.0.1"), e o 3DS recusava
instalar por cima. Este script escreve a versão (major << 10 | minor << 4 | micro) no
ticket e no TMD do .cia. As assinaturas de um .cia de homebrew já são falsas (o CFW
aceita), e a versão não entra nos hashes do conteúdo, então nada mais muda.

Uso: py versao_cia.py arquivo.cia 1.10.0
"""

import struct
import sys

TAM_ASSINATURA = {0x10003: 0x200 + 0x3C, 0x10004: 0x100 + 0x3C, 0x10005: 0x3C + 0x40}


def alinhar(x):
    return (x + 63) // 64 * 64


def corpo(dados, pos):
    """Onde começa o conteúdo de um bloco assinado (ticket ou TMD)."""
    tipo = struct.unpack(">I", dados[pos:pos + 4])[0]
    return pos + 4 + TAM_ASSINATURA[tipo]


def gravar_versao(caminho, versao):
    major, minor, micro = (int(p) for p in versao.split("."))
    if not (0 <= major < 64 and 0 <= minor < 64 and 0 <= micro < 16):
        raise SystemExit(f"versão fora do limite do 3DS: {versao} (major/minor até 63, micro até 15)")
    valor = major << 10 | minor << 4 | micro
    dados = bytearray(open(caminho, "rb").read())
    cabecalho, _, _, certs, ticket = struct.unpack("<IHHII", dados[:16])
    pos_ticket = alinhar(alinhar(cabecalho) + certs)
    pos_tmd = alinhar(pos_ticket + ticket)
    struct.pack_into(">H", dados, corpo(dados, pos_ticket) + 0xA6, valor)   # versão do ticket
    struct.pack_into(">H", dados, corpo(dados, pos_tmd) + 0x9C, valor)      # versão do título
    open(caminho, "wb").write(dados)
    print(f"{caminho}: versão {versao} ({valor})")


if __name__ == "__main__":
    gravar_versao(sys.argv[1], sys.argv[2])
