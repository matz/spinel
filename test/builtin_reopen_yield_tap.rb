# A tap, then or yield_self of a String, Symbol, Float or Range reopen:
# the method's own yield binds the block parameter.
class Symbol
  def then = yield(5)
  def tap = 7
end
class String
  def yield_self = yield(5)
  def tap = 8
end
class Float
  def tap = yield(5)
end
class Range
  def then = yield(5)
end

p :a.then { |x| x }
p :a.then { |x| x + 1 }
p :a.tap { |x| x }
p "a".yield_self { |x| x + 1 }
p "a".tap { |x| x }
p 1.5.tap { |x| x + 1 }
p (1..2).then { |x| x + 1 }
