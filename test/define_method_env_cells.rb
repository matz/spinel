# A define_method or define_singleton_method block in a method's body closes
# over that method's parameters and locals as it does in CRuby: the method it
# defines reads and writes the same variables the method has, through the
# environment its definer built, after the definer's method has returned. A
# write the method makes after the definer, and one the defined method makes,
# are seen on both sides; a second run of the definer gives the method an
# environment of its own, and a call already running keeps the one it started
# with.

# a parameter, a local the method changes, a write after the definer
class Plain
  def self.mk(n)
    base = 10
    base += 5
    define_method(:m) { |x| n * 100 + base + x }
    base = 20
    :ok
  end
end
Plain.mk(3)
p Plain.new.m(1)

# a write from the defined method, and a second method of the same environment
class Counter
  def self.mk
    n = 0
    define_method(:inc) { n += 1 }
    define_method(:get) { n }
    :ok
  end
end
Counter.mk
c = Counter.new
c.inc
p c.inc
p c.get, Counter.new.get

# a second run gives the next environment; the first one is gone with its method
class Rerun
  def self.mk(n)
    define_method(:m) { n }
    :ok
  end
end
Rerun.mk(1)
p Rerun.new.m
Rerun.mk(2)
p Rerun.new.m

# recursion through the defined method, and a call that runs the definer again
class Recur
  def self.mk(base)
    define_method(:f) { |n| n <= 0 ? base : n + f(n - 1) }
    :ok
  end
end
Recur.mk(100)
p Recur.new.f(3)
class Again
  def self.mk(n)
    define_method(:m) { Again.mk(n + 1); n }
    :ok
  end
end
Again.mk(1)
p Again.new.m
p Again.new.m

# nested blocks, an iteration block's parameter and local, a loop
class Nested
  def self.mk(a)
    b = 5
    define_method(:m) { |xs| xs.map { |x| x + a + b } }
    [1, 2].each do |it|
      t = it * 3
      define_method(:each_m) { [t, it] }
    end
    3.times { |i| define_method(:times_m) { i * 11 } }
    i = 0
    while i < 3
      j = i * 10
      define_method(:while_m) { j }
      i += 1
    end
    :ok
  end
end
Nested.mk(10)
n = Nested.new
p n.m([1, 2]), n.each_m, n.times_m, n.while_m

# define_singleton_method on the class, in two runs, and a subclass
class Single
  def self.mk_single(n)
    define_singleton_method(:g) { n }
    define_singleton_method(:h) { |a| a + n }
    :ok
  end
end
Single.mk_single(1)
p Single.g, Single.h(10)
Single.mk_single(2)
p Single.g, Single.h(10)
class SubSingle < Single; end
p SubSingle.g

# a String and an Array that the defined method changes, and other kinds of value
class Shared
  def self.mk(s, a, h)
    define_method(:m) { |v| s << "!"; a << v; h[v] = a.size; [s, a.dup, h.dup] }
    :ok
  end
end
Shared.mk(+"hi", [], {})
sh = Shared.new
p sh.m(5)
p sh.m(6)
class Kinds
  def self.mk(f, sym, nothing, l, r)
    define_method(:m) { [f, sym, nothing, l.call(2), r.to_a] }
    :ok
  end
end
Kinds.mk(2.5, :s, nil, ->(v) { v * 4 }, 1..3)
p Kinds.new.m
class Boxed
  def self.mk(v)
    define_method(:m) { v }
    :ok
  end
end
Boxed.mk(nil)
p Boxed.new.m
Boxed.mk([1, "a", :b])
p Boxed.new.m

# a captured block, a rest, a default and a keyword
class Params
  def self.mk(first, d = 4, *rest, k: 3, &blk)
    define_method(:m) { |x| [first, rest, d, k, blk.call(x)] }
    :ok
  end
end
Params.mk(1, 2, 3, 5, k: 6) { |v| v * 3 }
p Params.new.m(7)

# under a condition and in an ensure clause; the method before and after
class Branchy
  def self.mk(n)
    define_method(:m) { n } if n > 0
    define_method(:z) { -n } unless n > 0
    begin
      raise "x" if n > 1
    rescue
      n = -n
    ensure
      define_method(:e) { n * 2 }
    end
    :ok
  end
end
p((Branchy.new.m rescue $!.class))
Branchy.mk(2)
p Branchy.new.m, (Branchy.new.z rescue $!.class), Branchy.new.e
Branchy.mk(1)
p Branchy.new.m, Branchy.new.e

# the body of an endless method, a call before the definer runs, and the name sent
class Early
  def self.mk(n) = define_method(:m) { n }
end
p((Early.new.m rescue $!.class))
Early.mk(4)
e = Early.new
p e.m, e.send(:m), e.public_send(:m), e.respond_to?(:m)

# a block's own parameter or local that has a captured name is its own
class Own
  def self.mk(n)
    define_method(:own) { |n| n }
    :ok
  end
end
Own.mk(1)
p Own.new.own(7)

# the value of a method that ends in an operator write to a captured local
def tail_write
  v = 5
  pr = proc { v }
  v += 1
end
p tail_write

# a block parameter's default that reads a local the block closes over is evaluated in the method's environment
class Dflt
  def self.mk(n)
    define_method(:m) { |a, b = n, *r, &blk| [a, b, r, blk ? blk.call(n) : nil] }
    :ok
  end
end
Dflt.mk(3)
p Dflt.new.m(1)
p Dflt.new.m(1, 2, 3) { |x| x * 2 }
Dflt.mk(8)
p Dflt.new.m(1)

# a proc that a class method's block makes, in a subclass's call, closes over a captured local and reads self
class Cls
  def self.install(x)
    define_singleton_method(:m) do
      n = self.name
      -> { [x, n] }
    end
  end
end
class SubCls < Cls; end
Cls.install(1)
p SubCls.m.call
