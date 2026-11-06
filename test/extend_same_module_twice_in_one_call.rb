# `extend M1, M2, M1` extends M1, then M2: the first M1 stays behind M2, so
# the read runs M2's method; the later own def shadows both
module M1; def v = 1; end
module M2; def v = 2; end
class C; extend M1, M2, M1; end
puts C.v
def C.v = 3
puts C.v
