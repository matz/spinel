# A Class value that is a builtin class or a subclass of one answers `[]`,
# `new` with arguments, the class methods of a reopened builtin and `::X`
# through the plan's class-value arms, as the constant spelling does.

class Array; def self.hello = "arr"; X = 7; end
class MyA < Array; def self.hello = "mya"; X = 8; end
class MyB < Array; end
class MyH < Hash; end
class Object; def self.tag = "obj"; end
class Foo; def self.tag = "foo"; end
class Hash; def self.build(a, b = 2) = [a, b]; end
class String; def self.twice(s) = s + s; end

# `[]` on a Class value
def mk(k) = k[7, 8]
p mk(Array), mk(MyA), mk(MyA).class, mk(Array).class, mk(MyB).class
def mkh(k) = k[[[1, 2]]]
p mkh(MyH).class, mkh(MyH), mkh(Hash)
k = MyA
p k[1, 2], k[1, 2].class
def lit(v) = v.class[9, 8]
p lit([1]), lit(MyA.new), lit(MyA.new).class

# `new` with arguments on a Class value
def fill(k, n) = k.new(n, :x)
p fill(Array, 2), fill(MyA, 2), fill(MyA, 2).class
j = MyA
p j.new(2, 0), j.new(2, 0).class, j.new(3) { |i| i * 2 }
def dupe(v) = v.class.new(2, 0)
p dupe([1]), dupe(MyA.new).class, dupe(MyB.new).class
def pick(f) = f ? MyA : nil
p pick(true)&.new(2, 0), pick(false)&.new(2, 0)
h = MyH
p h.new(5).class, h.new(5)[:x]

# class methods of a reopened builtin, through a class value read off a value
def go(v) = v.class.hello
p go([1]), go(MyA.new), go(MyB.new)
def tag(v) = v.class.tag
p tag(Foo.new), tag(Object.new)
def viaparam(k) = k.tag
p viaparam(Foo), viaparam(Object)
def build(k) = k.build(1)
p build(Hash), build(MyH)
def twice(k) = k.twice("ab")
p twice(String)

# a constant through a class value
def cx(v) = v.class::X
p cx([1]), cx(MyA.new), cx(MyB.new)
[Array, MyA, MyB].each { |q| p q::X }

# a reopened method's receiver beside a subclass nobody uses
class Array; def kn = self.class.name; end
p [1].kn, MyA.new.size

# the instance a class value builds is a full instance of its class
class MyA; def t = "t#{size}"; end
class MyH; def t = "h#{size}"; end
p mk(MyA).t, fill(MyA, 2).t, mkh(MyH).t, h.new(5).t
class Stack < Array; def top = last; end
p mk(Stack).top, mk(Stack).class, fill(Stack, 3).size
class Typed < Array; end
ty = Typed.new
ty << 1 << 2
p ty.sum, mk(Typed).sum, fill(Typed, 3).size

# Hash[] with arguments CRuby refuses
def hh(k, a) = k[a]
[5, [[1, 2, 3]], [1, 2]].each do |bad|
  begin
    hh(MyH, bad)
  rescue ArgumentError => e
    p e.message
  end
end
