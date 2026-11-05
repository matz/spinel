# respond_to_missing? is asked when the Method is made, with
# include_private true for method and false for public_method; a real
# method of the name wins; a Method made before a later include keeps
# calling method_missing.
class Lazy
  LOG = []
  def initialize = @on = false
  def on! = @on = true
  def method_missing(name, *args) = name == :a ? [:mm, @on, args] : super
  def respond_to_missing?(name, include_private)
    LOG << [name, include_private]
    (name == :a && @on) || super
  end
end
z = Lazy.new
begin
  z.method(:a)
rescue NameError => e
  p [e.class, e.name]
end
z.on!
p z.method(:a).call(1)
z.public_method(:a)
p Lazy::LOG

class Priv
  def method_missing(name, *args) = name == :a ? :mm : super
  def respond_to_missing?(name, include_private = false) = name == :a || super
  private def a = :priv
end
p Priv.new.method(:a).call

module Late
  def a = :mod
end
class Inc
  def method_missing(name, *args) = name == :a ? :mm : super
  def respond_to_missing?(name, include_private = false) = name == :a || super
end
k = Inc.new
m = k.method(:a)
Inc.include(Late)
p m.call
p k.a
p k.method(:a).owner
p m.owner

# the receiver's own method_missing answers
class Base
  def method_missing(name, *args) = name == :a ? :base : super
  def respond_to_missing?(name, include_private = false) = name == :a || super
end
class Sub < Base
  def method_missing(name, *args) = name == :a ? :sub : super
end
p Base.new.method(:a).call
p Sub.new.method(:a).call

# a method_missing redefined after the capture answers the Method's later calls
class Redef
  def method_missing(name, *args) = name == :a ? :mm1 : super
  def respond_to_missing?(name, include_private = false) = name == :a || super
end
m = Redef.new.method(:a)
p m.call
class Redef
  def method_missing(name, *args) = name == :a ? :mm2 : super
end
p m.call
