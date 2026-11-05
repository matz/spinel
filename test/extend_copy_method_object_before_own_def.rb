# a Method object taken before the shadowing def keeps the extended method
module M; def v = 1; end
class C; extend M; end
m = C.method(:v)
def C.v = 2
puts m.call
puts C.method(:v).call
