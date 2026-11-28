# `instance_method(:m)` on self in a class method, bare or as
# `self.instance_method(:m)`: the class's own instance method, as
# `Klass.instance_method(:m)` names it. In a module's class-method mixin
# each extending class's copy answers for that class, so a subclass's
# override moves the owner.
module Serial
  module ClassMethods
    def compatible? = ancestors.index(instance_method(:ser).owner) <= ancestors.index(instance_method(:deser).owner)
    def deser_owner = self.instance_method(:deser).owner
  end
  def self.included(klass) = klass.extend(ClassMethods)
  def ser = "Serial#ser"
end

class Value
  include Serial
  def deser = "Value#deser"
  def self.ser_owner = instance_method(:ser).owner
  def self.ser_name = instance_method(:ser).name
end

class Sub < Value
  def ser = "Sub#ser"
end

p Value.compatible?
p Sub.compatible?
p Value.ser_owner
p Sub.ser_owner
p Value.ser_name
p Value.deser_owner
p Sub.deser_owner
[Value, Sub].each { |k| p k.compatible? }

# a subclass that overrides with an accessor: its reader is the method, arity 0
class Value
  def self.ser_arity = instance_method(:ser).arity
end
class Acc < Value
  attr_reader :ser
end
p Acc.ser_owner
p Acc.ser_arity
p Sub.ser_arity
