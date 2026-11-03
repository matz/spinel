# `extend self` is an extend of the module on itself, made where it runs
# (#10790): a method of the module is not one of its own before it, the
# module's `extended` hook runs for it, an attr of the module is the
# module's own too, and an accessor its singleton class declares answers
# before the instance method of the name.
module Late
  def a = 1
  p((Late.a rescue :nomethod))
  extend self
  p Late.a
end

module Hooked
  def self.extended(o) = puts("extended #{o}")
  extend self
  def hi = "hi"
end
puts Hooked.hi

module Attrs
  extend self
  attr_accessor :z
  def inst = :inst
end
Attrs.z = 3
p Attrs.z
class UsesAttrs; include Attrs; end
p UsesAttrs.new.inst

# a module's private method that `extend` brings in through an include
# stays private on the extender
module Secret
  def shown = secret
  private def secret = :s
end
module Carrier
  include Secret
end
class Ext; extend Carrier; end
p Ext.shown
begin
  Ext.secret
rescue NoMethodError => e
  puts e.message
end

module Both
  include Secret
  extend self
end
p Both.shown
begin
  Both.secret
rescue NoMethodError => e
  puts e.message
end
