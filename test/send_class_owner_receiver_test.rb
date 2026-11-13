# A boxed receiver is tested for the class or module whose own `send` answers
# it by the class itself, not by a method the value's class may define: a
# class of the value with its own `is_a?`, `<=` or `equal?` is not an owner.
# And a module's own `send` is the module's: a class including it does not
# have it, so a send on that class runs the named method.

class K
  def self.send(*a) = [:ksend, *a]
  def self.m = :km
end
class L
  def m = :lm
  def is_a?(x) = true
  def <=(x) = true
  def equal?(x) = true
end
y = [L.new, K][ARGV.size]
p y.send(:m)
z = [K, L.new][ARGV.size]
p z.send(:m)

module M
  def self.send(*a) = [:msend, *a]
  def self.m = :mm
end
class C
  include M
  def self.m = :cm
end
x = [C, M][ARGV.size]
p x.send(:m)
u = [M, C][ARGV.size]
p u.send(:m)
