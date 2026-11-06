# a read of the extended method, a singleton def, and a read after it
module M; def v = 1; end
class C; extend M; end
puts C.v
def C.v = 2
puts C.v
