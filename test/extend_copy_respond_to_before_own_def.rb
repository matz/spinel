# respond_to? and singleton_methods before and after the shadowing def
module M; def v = 1; end
class C; extend M; end
p C.respond_to?(:v)
p C.singleton_methods
puts C.v
def C.v = 2
p C.singleton_methods(false)
p C.singleton_methods
puts C.v
