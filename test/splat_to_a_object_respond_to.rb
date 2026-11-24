# A respond_to? the program defines on Object is asked for a class's splat
# like any other: it says no, so the object spreads itself and
# method_missing's to_a is not called. A blank slate (a BasicObject subclass)
# has no Object in its chain, so its own respond_to_missing? is asked
# directly and Object's respond_to? is not.
class Object
  def respond_to?(n, priv = false) = n == :to_a ? false : super
end

class C
  def method_missing(n, *a) = n == :to_a ? [:conv] : super
end

class Blank < BasicObject
  def respond_to_missing?(n, priv = false) = false
  def method_missing(n, *a) = n == :to_a ? [:bo, :bo2] : super
end

class BlankYes < BasicObject
  def respond_to_missing?(n, priv = false) = true
  def method_missing(n, *a) = n == :to_a ? [:bo, :bo2] : super
end

p [*C.new].map(&:class)
p [*Blank.new].size
p [*BlankYes.new]
