# Methods made from an ivar holding a class, beside an ivar holding an
# instance, an ivar holding either, a local and a constant.

class Box
  def self.pick(value) = 17
  def pick(value) = "instance"
end

class Other
  def self.pick(value) = 23
end

class ClassHolder
  def initialize
    @kind = Box
  end

  def picker = @kind.method(:pick)
  def picker_owner = @kind.method(:pick).owner
end

class InstanceHolder
  def initialize
    @kind = Box.new
  end

  def picker = @kind.method(:pick)
end

class EitherHolder
  def initialize(as_class)
    @kind = as_class ? Box : Box.new
  end

  def picker = @kind.method(:pick)
end

[ClassHolder.new.picker, InstanceHolder.new.picker,
 EitherHolder.new(true).picker, EitherHolder.new(false).picker].each do |m|
  p m.call(nil)
end
p [nil].map(&ClassHolder.new.picker)
p [nil].map(&InstanceHolder.new.picker)

kind = Box
from_ivar = ClassHolder.new.picker
from_local = kind.method(:pick)
from_const = Box.method(:pick)
p [from_ivar.arity, from_local.arity, from_const.arity]
p [ClassHolder.new.picker_owner, kind.method(:pick).owner, Box.method(:pick).owner]
p [from_ivar.call(1), from_local.call(1), from_const.call(1)]
p Other.method(:pick).call(nil)
