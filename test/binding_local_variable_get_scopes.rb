# binding.local_variable_get(:name) of a literal symbol reads the local the name
# is in scope as at that point: a block parameter, a block-local, a reserved-word
# keyword parameter, the enclosing method's parameter and locals from one to
# three blocks in, a lambda's parameter and the program's locals. A block
# parameter shadows an outer local of the name.
def method_param(a) = [1].map { |x| binding.local_variable_get(:x) + a }
p method_param(10)
p proc { |then:| binding.local_variable_get(:then) }.call(then: 4)
def outer_param(k)
  [1, 2].map { |x| binding.local_variable_get(:k) + x }
end
p outer_param(10)
p [1].map { |x; y| y = x * 3; binding.local_variable_get(:y) }
def three_levels(a)
  b = a * 2
  [1].map { |x| [2].map { |y| [3].map { |z|
    binding.local_variable_get(:b) + binding.local_variable_get(:a) + x + y + z } } }
end
p three_levels(5)
l = ->(m) { binding.local_variable_get(:m) * 2 }
p l.(21)
t = 7
l2 = -> { binding.local_variable_get(:t) }
p l2.()
p [1].map { |x| binding.local_variable_get(:t) + x }
class C
  define_method(:dm) { |q| binding.local_variable_get(:q) + 1 }
end
p C.new.dm(1)
x = 100
p [1].map { |x| binding.local_variable_get(:x) }, x
begin
  p proc { binding.local_variable_get(:_1) }.call(5)
rescue NameError => e
  puts e.message
end
