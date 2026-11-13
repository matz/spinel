# The NoMethodError a call raises for a method no class defines stages the
# arguments the call was given for #args. Each boxed argument that holds heap
# (a Range's box is a fresh copy) is rooted while the next is built, so a
# collection between them frees none. Through a boxed receiver (a value that
# may be an A or a B), and through a typed one.

class A
  def foo(a, b, c) = :foo
end
class B
  def bar(a, b, c) = :bar
end
x = [A.new, B.new][ARGV.size]
a = A.new
r1, r2, r3 = 1..2, 3..4, 5..6
bad = 0
2000.times do
  x.qux(r1, r2, r3)
rescue NoMethodError => e
  bad += 1 unless e.args == [1..2, 3..4, 5..6]
end
p bad

bad = 0
2000.times do |i|
  a.qux(r1, 7, r2, "s#{i}")
rescue NoMethodError => e
  bad += 1 unless e.args == [1..2, 7, 3..4, "s#{i}"] && e.name == :qux
end
p bad

begin
  x.qux(r3, 9)
rescue NoMethodError => e
  p e.args
  puts e.message
end
