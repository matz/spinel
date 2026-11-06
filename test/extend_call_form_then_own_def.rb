# `C.extend(M)` at the top level, read, then an own def
module M; def v = 1; end
class C; end
C.extend(M)
puts C.v
def C.v = 2
puts C.v
