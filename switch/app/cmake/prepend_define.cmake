# Prepend a single "#define <DEF> 1" line to a GLSL shader, right after its #version
# directive
#
# Usage: cmake -DIN=<src> -DDEF=<macro> -DOUT=<out> -P prepend_define.cmake

file(READ "${IN}" SRC)
string(REGEX REPLACE "(#version[^\n]*\n)" "\\1#define ${DEF} 1\n" SRC "${SRC}")
file(WRITE "${OUT}" "${SRC}")
