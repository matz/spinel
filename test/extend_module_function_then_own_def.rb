# extending a module of functions brings private copies; a later public own def shadows one
module M
  module_function
  def v = 1
end
class C
  extend M
  def self.t = v
end
puts C.t
def C.v = 2
puts C.v
puts C.t
