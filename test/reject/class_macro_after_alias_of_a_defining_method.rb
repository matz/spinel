# An alias of a program method that defines still may define: the macro
# after `go` waits for Box#boom through `bang`, refused.
module Macros
  def attribute(name) = define_method("#{name}_#{sfx}") { name.to_s }
end
class Box
  def boom = Base.define_singleton_method(:sfx) { "b" }
  alias bang boom
end
def go(x) = x.bang
class Base
  extend Macros
  def self.sfx = "a"
  go(Box.new)
  attribute :title
end
p Base.instance_methods(false).sort
