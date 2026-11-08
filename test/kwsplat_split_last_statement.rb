# A split call that is the last statement of its list, with its value unused (a modifier, a begin and ensure, a case branch, an iterator's block, a loop body, the program's end), becomes statements: a block that ends in a def or attr_reader has no value to assign.
E = {}
def sum(**k) = yield(k)
class K
  def self.each(**k) = yield(k)
  each(**E) { |k| def self.a1 = :a1 }
  each(**E) { |k| attr_reader :q }
  nil
end
p K.a1
sum(**E) { |k| def a3 = :a3 } if E
p a3
begin
  sum(**E) { |k| def a4 = :a4 }
ensure
  sum(**E) { |k| def a5 = :a5 }
end
p [a4, a5]
case 1
when 1 then sum(**E) { |k| def a6 = :a6 }
end
p a6
[1].each { sum(**E) { |k| def a7 = :a7 } }
p a7
i = 0
while i < 1
  i += 1
  sum(**E) { |k| def a8 = :a8 }
end
p a8
sum(**E) { |k| def a9 = :a9 }
