# A builtin's Method passed as a block is a lambda of the builtin's arity:
# yielded another count of values, it raises ArgumentError as CRuby does.
# The wrapper took the leading values and dropped the rest, so
# `[1].map(&5.method(:abs))` answered [5]; an Array yielded to a Method of
# two parameters did not compile.
def t
  yield
rescue ArgumentError => e
  e.message
end
p t { [1].map(&5.method(:abs)) }
m = 5.method(:abs)
p t { [1].map(&m) }
p t { [4, 8].each_with_index.map(&2.method(:fdiv)) }
p t { [4, 8].each_with_index.map(&2.method(:+)) }
p t { [1].map(&5.method(:between?)) }
p t { [[1, 9]].map(&5.method(:between?)) }
p t { {a: 1}.map(&5.method(:abs)) }
e = [1, 2].select { |x| x > 5 }
p t { e.map(&5.method(:abs)) }
p [1, 2].map(&4.method(:fdiv)), [3, 9, 5].select(&4.method(:<))
