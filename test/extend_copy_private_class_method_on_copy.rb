# private_class_method on the extended method, then a public own def
module M; def v = 1; end
class C; extend M; def self.t = v; end
puts C.v
class C; private_class_method :v; end
puts C.t
def C.v = 2
puts C.v
