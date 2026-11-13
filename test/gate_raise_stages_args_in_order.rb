# spinel: gc-stress
# The NoMethodError a call raises for a method no class defines runs the
# call's receiver and arguments left to right, as CRuby does, where an
# argument has setup that runs ahead of its place: an Array.new's size and
# allocation, or an element's `+=`, no longer runs before the arguments to
# its left, or before a receiver that runs code (a constant the program
# does not define raises NameError before any argument runs) or a local the
# argument reassigns. A value an argument builds in the list (a lambda) is
# held while the next one is built.

def t(x) = (puts "t #{x}"; x)

class Foo; def hi = 1; end
class Bar; def hi = 2; end
def mk(i) = (puts "mk #{i}"; i == 0 ? Foo.new : Bar.new)

begin
  1.missing(t(1), Array.new(t(2)))
rescue NoMethodError => e
  p e.args
end

[0, 1].each do |i|
  mk(i).nope(t(3), Array.new(t(2)), [t(4)])
rescue NoMethodError => e
  p [e.receiver.class, e.args]
end

begin
  Foo.new.nope(t(5), [t(6), t(7)], Array.new(t(1)) { |k| k })
rescue NoMethodError => e
  p e.args
end

a = 1
begin
  2.missing(a, (a = 5; Array.new(t(2))), a)
rescue NoMethodError => e
  p e.args
end

ib = [1, 2]
begin
  1.missing(ib[0], (ib[0] += 5))
rescue NoMethodError => e
  p e.args
end

fa = [1.5, 2.5]
begin
  1.missing(fa[0], (fa[0] -= 1), fa[0])
rescue NoMethodError => e
  p e.args
end

begin
  t(1).missing(Array.new(t(2)))
rescue NoMethodError => e
  p e.args
end

x = 1
begin
  x.missing(Array.new((x = 2; t(3))))
rescue NoMethodError => e
  p [e.receiver, e.args]
end

s = +"ab"
begin
  Missing.build(t(6), s << "c")
rescue NameError => e
  p [e.class, s]
end
begin
  Missing.build(t(1), -> { 1 })
rescue NameError => e
  p e.class
end
begin
  Missing.build(t(1), t(2))
rescue NameError => e
  p e.class
end
begin
  Missing.build(t(1), [2])
rescue NameError => e
  p e.class
end

v = [1, "a", nil][0]
bad = 0
300.times do |i|
  v.missing(-> { i }, -> { i + 1 }, -> { i + 2 }, -> { i + 3 })
rescue NoMethodError => e
  bad += 1 unless e.args.map(&:call) == [i, i + 1, i + 2, i + 3]
end
p bad
