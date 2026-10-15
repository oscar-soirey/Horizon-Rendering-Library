@echo off
rem Regenerates the Python, C#, Rust, Go and LuaJIT bindings from the C headers (needs Python 3.8+).
rem   generate-bindings.bat              all languages
rem   generate-bindings.bat python rust  some of them
python "%~dp0bindings\generate.py" %*
