# An alias of a builtin that runs code (send) still may define: the macro
# after `go` waits for Box#boom, refused.
module Macros
  def attribute(name) = define_method("#{name}_#{sfx}") { name.to_s }
end
class Box
  alias_method :run_it, :send
  def boom = Base.define_singleton_method(:sfx) { "b" }
end
def go(x, m) = x.run_it(m)
class Base
  extend Macros
  def self.sfx = "a"
  go(Box.new, :boom)
  attribute :title
end
p Base.instance_methods(false).sort
