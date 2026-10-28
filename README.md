# Radio 3DS

Rádio pela internet no 3DS (Old e New). Gire o globo, centralize uma cidade e ouça as rádios de lá.
São 6.928 rádios MP3 em 3.449 cidades, com dados do radio-browser.info e do GeoNames.

## Instalar

1. O 3DS precisa de CFW (Luma3DS) com o Homebrew Launcher.
2. O arquivo `sdmc:/3ds/dspfirm.cdc` precisa existir (rode o **DSP1** uma vez). Sem ele não há som.
3. Copie `Radio3DS.3dsx` para a pasta `3ds/` do cartão SD.
4. Abra pelo Homebrew Launcher.

## Controles

| Botão | Ação |
|---|---|
| Direcional circular | Girar o globo |
| L / R (C-stick no New 3DS) | Zoom |
| Direcional ← → | Trocar de aba |
| Direcional ↑ ↓ | Navegar na lista (na aba Tocando: volume) |
| A | Ouvir a cidade da mira / tocar o item selecionado |
| B | Parar |
| X | Favoritar / desfavoritar |
| Y | Rádio surpresa (na aba Busca: abrir teclado) |
| SELECT | Normal → modo relógio → telas apagadas |
| START | Sair |

## Alarme e modo repouso

- Aba do relógio: liga o alarme, define a hora, escolhe a rádio (um favorito ou a última ouvida) e o temporizador para dormir.
- O volume do alarme sobe aos poucos durante 1 minuto. Se a rádio não conectar em 20 s, toca um bipe.
- Com o alarme ligado, o app impede o 3DS de dormir. Fechar a tampa apaga as telas, mas o app continua rodando.
  Deixe o console no carregador e com o volume físico alto.
- O modo de espera real do 3DS congela todos os apps, então nenhum homebrew consegue tocar alarme nele.
- O sistema do 3DS desliga o alto-falante quando a tampa fecha (o fone continua tocando). A opção
  **Som com a tampa fechada** (aba do alarme) é experimental: ela tenta religar o alto-falante e anota o que
  aconteceu em `sdmc:/3ds/Radio3DS/codec.log`. Se não funcionar no seu console, deixe a tampa aberta e use o
  modo de telas apagadas (SELECT duas vezes).

## Suas próprias rádios

Edite `sdmc:/3ds/Radio3DS/favorites.tsv`, uma rádio por linha:

```
Nome da rádio<TAB>http://endereco-do-stream<TAB>-1
```

## Compilar

Precisa do devkitPro com `3ds-dev`, `3ds-mpg123`, `3ds-curl`, `3ds-mbedtls` e `3ds-zlib`. Depois é só rodar `make`.

Para gerar o `.cia`, rode `make cia`. O comando usa `tools/bin/makerom.exe`
([Project_CTR v0.19.0](https://github.com/3DSGuy/Project_CTR/releases)) e `tools/bin/bannertool.exe`
([diasurgical 1.2.0](https://github.com/diasurgical/bannertool/releases)). Eles não vão no repositório: baixe e coloque em
`tools/bin/`. Instale o `.cia` no 3DS com o FBI.

Versão nova: aumente `VERSION_MAJOR/MINOR/MICRO` no `Makefile`, rode `make cia`, depois
`python tools/package.py . Radio3DS` para montar `dist/Radio3DS-pacote.zip`. Publique uma release `vX.Y.Z` no GitHub com o
`.3dsx`, o `.cia` e o pacote: o app instala a nova versão sozinho (Ajustes > Atualizar).

Medidor de desempenho: crie um arquivo vazio `sdmc:/3ds/Radio3DS/perf`. O tempo de cada quadro aparece em verde
na tela de cima e é salvo em `sdmc:/3ds/Radio3DS/perf.log`. Apague o arquivo para desligar.

Lista de rádios: `pc/atualizar_radios.bat` baixa os dados do radio-browser e do GeoNames, gera `cities.tsv` e
`stations.tsv` em `pc/radios/` e copia para `sdmc:/3ds/Radio3DS/`, que o app usa no lugar da lista embutida. Para trocar a
lista embutida, copie os dois arquivos para `romfs/` e recompile.
