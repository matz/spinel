# A program that reopens BasicObject (`class BasicObject; end`) makes it the
# top of every chain. A super that passes it does not reach Object's method,
# since Object is not an ancestor of a BasicObject subclass.
class Object; def zed = "oz"; end
module Kernel; def kz = "kz"; end
class BasicObject; def bz = "bz"; end
class B1 < BasicObject
  def zed = super
  def kz = super
  def bz = super + "!"
end
class BasicObject; def own = super; end
def try(n)
  r = yield
  puts "#{n}: #{r}"
rescue NoMethodError
  puts "#{n}: NoMethodError"
end
try(:object_method) { B1.new.zed }
try(:kernel_method) { B1.new.kz }
try(:basic_object_method) { B1.new.bz }
try(:own_super_on_string) { "s".own }
try(:object_method_on_string) { "s".zed }
