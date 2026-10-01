# A block instance_exec or instance_eval runs reads its receiver's ivars,
# as in CRuby: a value with no ivar layout (nil, a builtin value,
# Object.new) has none, so they read nil, and so does one the receiver's
# class never writes. Spinel read the caller's ivars, or built C that
# named a field the class does not have. The arguments still read the
# caller's.

@d = 0
p Object.new.instance_exec { @d }
p Object.new.instance_eval { @d }
p nil.instance_exec { @d }, 5.instance_exec { @d }, "s".instance_exec { @d }
p nil.instance_eval { @d }, 5.instance_eval { @d }, :s.instance_eval { @d }
p Object.new.instance_exec(@d) { |a| [a, @d] }
p 5.instance_exec(@d) { |a, k: @d| [a, k, @d] }
p Object.new.instance_exec(1, *[2]) { |a, b, k: @d| [a, b, k] }
p [1, :s, 2.0][1].instance_exec { @d }
p [1, :s, 2.0][0].instance_exec(@d) { |q| [q, @d] }

class K; def initialize = (@e = 1); end
p K.new.instance_exec { [@d, @e] }

class Q
  def initialize = (@d = 5)
  def go = Object.new.instance_exec(@d) { |a| [a, @d] }
  def own = instance_exec { @d }
end
p Q.new.go, Q.new.own

class W; end
o = W.new
o.instance_exec { @w = 3 }
p o.instance_exec { @w }, @d

# an ivar nested deeper than the splice looks still reads the receiver's
p [1].instance_exec { [[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[@d]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]] }.flatten

# a nested instance_exec on an object writes that object's ivar
class Rv
  def initialize = (@value = 0)
  def value = @value
end
rv = Rv.new
Object.new.instance_exec { rv.instance_exec { @value = 1 } }
p rv.value
