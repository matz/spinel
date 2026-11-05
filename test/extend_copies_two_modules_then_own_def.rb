# two extends in turn, each read, then an own def
module M1; def v = 1; end
module M2; def v = 3; end
class C; extend M1; end
puts C.v
class C; extend M2; end
puts C.v
def C.v = 2
puts C.v
