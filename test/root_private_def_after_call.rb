# A call with an explicit receiver that runs before a private Kernel,
# Object or Numeric def of a builtin's name (tap, then, frozen?, dup) runs the
# builtin: the def is not there yet. From the def on, the call raises
# CRuby's private-method NoMethodError. Receivers nil, true, an Integer,
# a Float, a String, an Array, a program object and a class; the
# `private def` and `private :name` forms and public_send.

class Foo; end

begin; p(nil.tap { |x| puts "nil tap #{x.inspect}" }); rescue NoMethodError => e; puts e.message; end
begin; p(1.tap { |x| puts "int tap #{x}" }); rescue NoMethodError => e; puts e.message; end
begin; p(true.tap { |x| puts "true tap" }); rescue NoMethodError => e; puts e.message; end
begin; p("s".tap { |x| puts "str tap" }); rescue NoMethodError => e; puts e.message; end
begin; p([1].tap { |x| puts "ary tap" }); rescue NoMethodError => e; puts e.message; end
begin; p(Foo.new.tap { |x| puts "obj tap" }.class); rescue NoMethodError => e; puts e.message; end
begin; p(Foo.tap { |x| puts "class tap" }); rescue NoMethodError => e; puts e.message; end
begin; p(nil.public_send(:tap) { |x| puts "nil public_send tap" }); rescue NoMethodError => e; puts e.message; end
begin; p(nil.then { |x| x.inspect }); rescue NoMethodError => e; puts e.message; end
begin; p(1.frozen?); rescue NoMethodError => e; puts e.message; end
begin; p(2.5.dup); rescue NoMethodError => e; puts e.message; end

module Kernel
  private
  def tap; :kernel_tap; end
end
class Object
  private def then; :object_then; end
end
class Object
  def frozen?; :object_frozen; end
  private :frozen?
end
class Numeric
  private
  def dup; :numeric_dup; end
end

begin; p(nil.tap { |x| puts "nil tap #{x.inspect}" }); rescue NoMethodError => e; puts e.message; end
begin; p(1.tap { |x| puts "int tap #{x}" }); rescue NoMethodError => e; puts e.message; end
begin; p(true.tap { |x| puts "true tap" }); rescue NoMethodError => e; puts e.message; end
begin; p(Foo.new.tap { |x| puts "obj tap" }); rescue NoMethodError => e; puts e.message; end
begin; p(nil.public_send(:tap) { |x| puts "nil public_send tap" }); rescue NoMethodError => e; puts e.message; end
begin; p(nil.then { |x| x.inspect }); rescue NoMethodError => e; puts e.message; end
begin; p(1.frozen?); rescue NoMethodError => e; puts e.message; end
begin; p(2.5.dup); rescue NoMethodError => e; puts e.message; end
p nil.send(:tap)
