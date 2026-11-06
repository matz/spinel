# a method body reads the extended method before and after the singleton def
module M; def v = 1; end
class C; extend M; end
def g = C.v
puts g
def C.v = 2
puts g
