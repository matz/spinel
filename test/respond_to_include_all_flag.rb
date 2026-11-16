# respond_to? with an include_all that is no literal answers by the flag's
# truthiness, as the two literal answers: a public and a private top-level
# def, a class that defines the name privately or as its own, a class with
# respond_to_missing?, a visibility flip, a builtin value. The receiver and
# the flag run once, in that order.
def f = 1
def g = 4
class Foo
  def f = 2
  private def g = 3
end
class R
  def respond_to_missing?(n, p) = n == :zz
end
class Q
  def respond_to_missing?(n, p) = p && n == :zq
end
def h = 5
public :h
private :h
def pub = 6
public :pub
no = !ARGV.empty?
yes = ARGV.empty?
p 3.respond_to?(:f, no), 3.respond_to?(:f, yes)
p Foo.new.respond_to?(:f, no), Foo.new.respond_to?(:g, no), Foo.new.respond_to?(:g, yes)
p 3.respond_to?(:g, no), 3.respond_to?(:g, yes)
p R.new.respond_to?(:zz, no), R.new.respond_to?(:zz, yes), 3.respond_to?(:f, no)
p Q.new.respond_to?(:zq, no), Q.new.respond_to?(:zq, yes)
p 3.respond_to?(:h, no), 3.respond_to?(:h, yes)
p "s".respond_to?(:pub, no), "s".respond_to?(:pub, yes)
p [1, nil].map { |o| o.respond_to?(:puts, no) }, [1, nil].map { |o| o.respond_to?(:puts, yes) }
p 3.respond_to?(:f, 7), 3.respond_to?(:f, :x), Foo.new.respond_to?(:g, 0)
$l = []
def recv = ($l << :recv; 3)
p recv.respond_to?(:f, ($l << :flag; no)), $l
p respond_to?(:f, yes), respond_to?(:f, no)

# a builtin's private method answers by the flag too, as does a private new
class Priv; private_class_method :new; end
p 3.respond_to?(:initialize, no), 3.respond_to?(:initialize, yes)
p Object.new.respond_to?(:method_missing, no), Object.new.respond_to?(:method_missing, yes)
p Priv.respond_to?(:new, no), Priv.respond_to?(:new, yes)
p Object.respond_to?(:remove_const, no), Object.respond_to?(:remove_const, yes)

# a call through send and public_send is the same call
p 3.send(:respond_to?, :puts, no), 3.send(:respond_to?, :puts, yes)
p 3.public_send(:respond_to?, :puts, no), 3.public_send(:respond_to?, :puts, yes)

# the flag can rebind the local the receiver reads
rv = "a"
p rv.respond_to?(:upcase, (rv = 3; yes)), rv

# a builtin the program redefines runs its code: a receiver read after a
# `method` the program defines (or a `proc`, a `lambda`) in the flag sees what
# that code left, so the receiver is held first
class W
  def initialize = (@receiver = "a")
  def method(_) = (@receiver = []; true)
  def go = @receiver.respond_to?(:upcase, method(:ignored))
end
p W.new.go
class W2
  def initialize = (@receiver = "a")
  def proc = (@receiver = []; true)
  def lambda = (@receiver = []; true)
  def go1 = @receiver.respond_to?(:upcase, proc { 1 })
  def go2 = @receiver.respond_to?(:upcase, lambda { 1 })
end
p W2.new.go1, W2.new.go2

# a Hash or Range literal over the program's own objects runs their `hash` and
# `<=>`: the receiver is held first
class Key
  def initialize(o) = (@o = o)
  def hash = (@o.swap; 1)
  def eql?(x) = true
  def <=>(x) = (@o.swap; 0)
end
class W3
  def initialize = (@receiver = "a")
  def swap = (@receiver = [])
  def go_hash = @receiver.respond_to?(:upcase, {Key.new(self) => 1})
  def go_hash_local = (k = Key.new(self); @receiver.respond_to?(:upcase, {k => 1}))
  def go_range = (k = Key.new(self); @receiver.respond_to?(:upcase, (k..k)))
end
p W3.new.go_hash, W3.new.go_hash_local, W3.new.go_range
