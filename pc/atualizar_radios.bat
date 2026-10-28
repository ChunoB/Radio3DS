@echo off
chcp 65001 >nul
title Atualizar radios do Radio 3DS
echo === Atualizar a lista de radios do Radio 3DS ===
echo.
where py >nul 2>nul
if errorlevel 1 (
    echo Python nao encontrado.
    echo Instale o Python 3 em https://www.python.org/downloads/
    echo e marque "Add Python to PATH" durante a instalacao.
    echo.
    pause
    exit /b 1
)
echo Conecte o cartao SD do 3DS no PC antes, se quiser que a lista
echo seja copiada sozinha.
echo.
py -3 "%~dp0atualizar_radios.py"
echo.
pause
