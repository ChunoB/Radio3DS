"""Atualiza a lista de radios do Radio 3DS.

Baixa as radios MP3 do radio-browser.info e as cidades/fusos do GeoNames,
gera cities.tsv e stations.tsv e copia para o cartao SD (pasta 3ds/Radio3DS),
se ele estiver conectado ao PC. O app usa essa lista no lugar da que vem dentro dele.
Precisa so do Python 3 (sem bibliotecas extras).
"""
import os
import shutil
import string
import sys
import urllib.request

AQUI = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, AQUI)
import build_db  # noqa: E402

PASTA = os.path.join(AQUI, "radios")
FONTES = {
    "stations.json": "https://de1.api.radio-browser.info/json/stations/search"
                     "?codec=MP3&has_geo_info=true&hidebroken=true&limit=100000",
    "cities15000.zip": "https://download.geonames.org/export/dump/cities15000.zip",
    "timeZones.txt": "https://download.geonames.org/export/dump/timeZones.txt",
}


def baixar(nome, url):
    destino = os.path.join(PASTA, nome)
    print(f"Baixando {nome}...")
    req = urllib.request.Request(url, headers={"User-Agent": "Radio3DS-pc/1.0"})
    with urllib.request.urlopen(req, timeout=120) as r, open(destino, "wb") as f:
        shutil.copyfileobj(r, f)
    return destino


def achar_sd():
    """Drives que parecem o cartao SD do 3DS (tem a pasta 3ds na raiz)."""
    achados = []
    for letra in string.ascii_uppercase:
        raiz = f"{letra}:\\"
        if letra in "C" or not os.path.isdir(raiz):
            continue
        if os.path.isdir(os.path.join(raiz, "3ds")) or os.path.isdir(os.path.join(raiz, "Nintendo 3DS")):
            achados.append(raiz)
    return achados


def main():
    os.makedirs(PASTA, exist_ok=True)
    arquivos = {nome: baixar(nome, url) for nome, url in FONTES.items()}
    print("Montando a lista...")
    build_db.load_tz(arquivos["timeZones.txt"])
    build_db.main(arquivos["stations.json"], arquivos["cities15000.zip"], PASTA)

    sds = achar_sd()
    if not sds:
        print()
        print("Nao achei o cartao SD do 3DS conectado.")
        print(f"Copie cities.tsv e stations.tsv de {PASTA}")
        print("para a pasta 3ds\\Radio3DS do cartao SD.")
        return
    for sd in sds:
        alvo = os.path.join(sd, "3ds", "Radio3DS")
        os.makedirs(alvo, exist_ok=True)
        for nome in ("cities.tsv", "stations.tsv"):
            shutil.copy2(os.path.join(PASTA, nome), os.path.join(alvo, nome))
        print(f"Lista copiada para {alvo}")
    print("Pronto! Abra o Radio 3DS para usar a lista nova.")


if __name__ == "__main__":
    main()
