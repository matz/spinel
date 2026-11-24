# A send of a computed name that passes nothing still runs a method the
# record cannot name: `Box.new.send(m)` may run Box#boom, which defines, so
# the macro after `go` waits for it, refused (the send's own answer).
module Macros
  def attribute(name) = define_method("#{name}_#{sfx}") { name.to_s }
end
class Box
  def boom
    Base.define_singleton_method(:sfx) { "b" }
    self
  end
end
class Base
  extend Macros
  def self.sfx = "a"
  def self.go(m) = Box.new.send(m)
  go(ARGV[0]&.to_sym || :boom)
  attribute :title
end
p Base.instance_methods(false).sort
