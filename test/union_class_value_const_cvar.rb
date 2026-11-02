# A boxed value that can be a class or another value answers, when it is
# the class, `const_defined?` and the class-variable reflection as the
# class called directly does: its own, an ancestor's and a module's
# constants, and the class variables its body or an ancestor's writes, read
# or set. The class raised NoMethodError, as if it were the other value. An
# Integer in the same slot still raises CRuby's NoMethodError.
class A; KA = 1; @@base = 1; end
class X < A
  K = 1
  @@v = 3
end
module M; K2 = 2; end

d = rand < 2 ? X : 5
p d.class_variable_set(:@@v, 7)
c = rand < 2 ? X : 5
p c.const_defined?(:K)
p c.const_defined?(:KA)
p c.const_defined?(:Nope)
p c.class_variable_get(:@@v)
p c.class_variable_get(:@@base)
p c.class_variable_defined?(:@@v)
p c.class_variable_defined?(:@@nope)
m = rand < 2 ? M : 5
p m.const_defined?(:K2)
n = rand < 2 ? X : nil
p n.class_variable_get("@@v")

i = rand < 0 ? X : 5
[-> { i.const_defined?(:K) }, -> { i.class_variable_get(:@@v) }].each do |f|
  f.call
rescue NoMethodError => e
  puts e.message
end
