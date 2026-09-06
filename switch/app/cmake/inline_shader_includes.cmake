# Resolve quoted #include lines in a GLSL shader by inlining the referenced files.
#
# Usage: cmake -DIN=<template> -DFFX_A=<ffx_a.h> -DFFX_CAS=<ffx_cas.h> -DOUT=<out> -P inline_shader_includes.cmake

file(READ "${IN}" SRC)
file(READ "${FFX_A}" FFX_A_SRC)
file(READ "${FFX_CAS}" FFX_CAS_SRC)
string(REPLACE "#include \"ffx_a.h\"" "${FFX_A_SRC}" SRC "${SRC}")
string(REPLACE "#include \"ffx_cas.h\"" "${FFX_CAS_SRC}" SRC "${SRC}")
file(WRITE "${OUT}" "${SRC}")
