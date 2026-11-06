# the singleton aliases the extended method, then the name is shadowed: the alias keeps the module's body
module M; def v = 1; end
class C
  extend M
  class << self; alias_method :w, :v; end
end
puts C.w
def C.v = 2
puts C.v
puts C.w
