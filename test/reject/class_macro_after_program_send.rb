# A send of a name the source spells runs the method of that name, or a
# program's own method of the send's name where the receiver has one:
# Box#send defines, so the macro after `go` waits for it, refused as before.
module Macros
  def attribute(name) = define_method("#{name}_#{sfx}") { name.to_s }
end
class Box
  def send(m, *a)
    Base.define_singleton_method(:sfx) { "b" }
    m
  end
end
class Base
  extend Macros
  def self.sfx = "a"
  def self.go = Box.new.send(:noop)
  go
  attribute :title
end
p Base.instance_methods(false).sort
