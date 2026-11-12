# `recv.method(:name)` on a builtin receiver (an Integer, Float, String,
# Symbol, Array or Hash) names a method only the program's reopening of the
# receiver's class, or of an ancestor of it, defines: the Method binds that def, as a call of the
# name does. The builtin tables do not list the name, so the capture raised
# NameError although the def was live where the Method is made. A name no
# reopening has defined yet (or ever) still raises there.

begin
  3.method(:late)
rescue NameError => e
  puts "before the def: #{e.class}"
end

class Integer
  def late = 1
  def double = self * 2
  def add(a, b = 10) = self + a + b
end
class Float; def half = self / 2; end
class String; def shout = upcase + "!"; end
class Symbol; def twice = to_s * 2; end
class Array; def second = self[1]; end
class Hash; def first_key = keys.first; end

m = 3.method(:double)
puts m.call, m.name.inspect, m.receiver
puts 5.method(:add).call(1), 5.method(:add).call(1, 2)
puts 3.method(:late).call
puts 3.0.method(:half).call
puts "hi".method(:shout).call
puts :ab.method(:twice).call.inspect
puts [4, 5, 6].method(:second).call
puts({ a: 1, b: 2 }.method(:first_key).call.inspect)

# a name of one class only: another class's receiver still raises
begin
  "x".method(:double)
rescue NameError => e
  puts "String has none: #{e.class}"
end
begin
  3.method(:nothing_here)
rescue NameError => e
  puts "undefined: #{e.class}"
end

# a module the class prepends, an attr_accessor on it
module Loud; def loud2 = self * 2; end
class Integer
  prepend Loud
end
class Array; attr_accessor :tag; end
puts 3.method(:loud2).call
a = [1]
a.tag = :t
puts a.method(:tag).call.inspect

# a name only the reopening of an ancestor defines (Numeric, Object)
class Numeric; def num_name = :numeric; end
class Object; def obj_name = :object; end
puts 3.method(:num_name).call, 2.5.method(:num_name).call
puts 3.method(:obj_name).call, "s".method(:obj_name).call, [1].method(:obj_name).call

# its reflection is the def's, its owner the class or module that holds it
class Numeric; def nn(a) = a; end
class Object; def onm(a, b = 1) = a; end
mn = 3.method(:nn)
puts mn.arity, mn.owner, mn.parameters.inspect
mo = "s".method(:onm)
puts mo.arity, mo.owner, mo.parameters.inspect, mo.call(7)

# public_method answers a public def only; method answers any
class Integer
  private def pv = :pv
  protected def pt = :pt
end
puts 3.method(:pv).call
begin; 3.public_method(:pv); puts "no raise"; rescue NameError => e; puts "public_method pv: #{e.class}"; end
begin; 3.public_method(:pt); puts "no raise"; rescue NameError => e; puts "public_method pt: #{e.class}"; end
begin; "s".public_method(:pv); puts "no raise"; rescue NameError => e; puts "String public_method pv: #{e.class}"; end

# public_method reads the visibility where it is made: a `public` that follows
# does not make the earlier Method
class Integer; private def v2 = :v2; end
begin; 3.public_method(:v2); puts "no raise"; rescue NameError => e; puts "before public: #{e.class}"; end
class Integer; public :v2; end
puts 3.public_method(:v2).call

# a true or false whose class is not known: the walk ends at TrueClass or
# FalseClass, Object's def of the name does not shadow theirs
class Object; def &(o, z = 1) = :obj; def to_s(q = 1) = "objts"; end
x = ARGV.size == 0
puts x.method(:&).arity, x.method(:to_s).arity

# the Method is also what send and a block of the name reach
puts 3.send(:double), 3.public_send(:double), [1, 2].map(&:double).inspect
