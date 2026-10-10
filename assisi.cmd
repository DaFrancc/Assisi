@echo off
rem Windows has no shebang, so this hands the tool to Python: assisi.cmd build
python "%~dp0assisi" %*
