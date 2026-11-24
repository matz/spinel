# A send of inject or reduce calls the method its last argument names on
# the accumulator as a direct call does: `send(:inject, :plus)` runs x.plus(1),
# which defines here, so the macro after `go` waits for it, refused (it was
# taken, and the macro read the old sfx).
module Macros
  def attribute(name) = define_method("#{name}_#{sfx}") { name.to_s }
end
class Box
  def plus(o)
    Base.define_singleton_method(:sfx) { "b" }
    self
  end
end
class Base
  extend Macros
  def self.sfx = "a"
  def self.go = [Box.new, 1].public_send(:inject, :plus)
  go
  attribute :title
end
p Base.instance_methods(false).sort
