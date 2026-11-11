# A top-level def is a private method of Object: a call with an explicit
# receiver finds it ahead of Kernel's method of the name and raises
# NoMethodError, as does a private method of a Kernel or Numeric reopening.
S = Struct.new(:a)
class K; end

def tap
  puts "my tap"
  self
end
def itself = :mine
def foo = :foo

def try
  yield
rescue NoMethodError => e
  puts "NoMethodError: #{e.message}"
end

try { p(1.tap { |x| puts "blk #{x}" }) }
try { p("s".tap { |x| puts "blk #{x}" }) }
try { p([1].itself) }
try { p(nil.itself) }
try { p(true.foo) }
try { p(false.foo) }
try { p(:s.foo) }
try { p(1.5.foo) }
try { p({ a: 1 }.foo) }
try { p((1..2).foo) }
try { p(K.new.foo) }
try { p(S.new(1).foo) }
try { 2.tap { |x| puts "stmt #{x}" } }
x = 3
try { p(x&.foo) }
y = nil
p y&.foo
try { p(1.public_send(:foo)) }
p foo, itself
[4, "t"].each { |v| try { p(v.foo) } }
def from_method(v) = v.foo
try { from_method(5) }

# the later of a top-level def and an Object def of a name answers
class Object
  def bar = :o
end
p 1.bar
def bar = :top
try { p(1.bar) }
def baz = :top
try { p(1.baz) }
class Object
  def baz = :o
end
p 1.baz

# Kernel's and Numeric's private methods
module Kernel
  private def kpriv = :k
end
try { p(1.kpriv) }
class Numeric
  private def npriv = :n
end
try { p(1.npriv) }
try { p(2.5.npriv) }

# a BasicObject has no Object method
class B < BasicObject; end
try { p(B.new.foo) }
try { p(BasicObject.new.foo) }

# the builtin's own answers: a Set's to_set is the Set (the identity the
# compiler writes for it), a class's `class << self` accessor its value
p Set[1, 2].to_set
def lvl = :top
class Bar
  class << self
    attr_accessor :lvl
  end
end
Bar.lvl = 4
p Bar.lvl
# the refused call's arguments run and are kept on the error
begin
  1.lvl("a" + "b", 2)
rescue NoMethodError => e
  p [e.message, e.name, e.args, e.receiver]
end

# the receiver's own class answers first: a Struct member's writer beside
# a top-level def of the setter, and beside an Object reopening's private one
def a=(v)
  :top
end
s = S.new(1)
s.a = 2
p s.a
class Object
  private def wm=(v)
    :o
  end
end
W = Struct.new(:wm)
w = W.new(1)
w.wm = 3
p w.wm
# a refused call whose block value is kept
def kept
  r = yield
  p r
end
kept { :s }
try { kept { 1.foo } }
kept { nil }
# a receiver that may be nil is named as the value is
v = ARGV.empty? ? nil : 1
try { p(v.foo) }
# a call in a method body that runs before the top-level def finds none
def calls_late(v) = v.late
try { calls_late(1) }
def late = :late
