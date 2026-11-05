# a call reads the extended copy, then a later class method supers into it
module M; def v = 1; end
class C; extend M; end
puts C.v
def C.v = super + 10
puts C.v
