@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64
echo LIB=%LIB%
echo ---
cargo build --release 2>&1
echo EXIT: %ERRORLEVEL%
