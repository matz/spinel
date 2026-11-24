# A nested send of spelled names runs a program's own method of each send's
# name on the way: Box#public_send defines, reached through
# `__send__(:public_send, :noop)`, so the macro after `go` waits for it,
# refused as before.
module Macros
  def attribute(name) = define_method("#{name}_#{sfx}") { name.to_s }
end
class Box
  def public_send(m, *a)
    Base.define_singleton_method(:sfx) { "b" }
    m
  end
end
class Base
  extend Macros
  def self.sfx = "a"
  def self.go = Box.new.__send__(:public_send, :noop)
  go
  attribute :title
end
p Base.instance_methods(false).sort
