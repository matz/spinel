# A library that renders no Symbol and no class name: its C defines
# SP_TU_NO_POLY_RENDER. The header's stand-ins for the renderers this
# leaves out were static, against the library's extern declarations of the
# same functions, so the library's C did not compile.
def twice(x) = x * 2
puts "loaded #{twice(21)}"
