# the extended module has the method through its own include
module A; def v = 1; end
module M; include A; end
class C; extend M; end
puts C.v
def C.v = 2
puts C.v
