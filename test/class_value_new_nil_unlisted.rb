# A nil handed to `new` on a Class value the analysis cannot list (one read
# out of an Array by an index only the run time knows) reaches each class's
# initialize: an Integer, a Float, an object or a keyword parameter takes it,
# where the class's constructor arm was left out and `new` raised
# NoMethodError. A nil an instance variable nothing sets reads is one too,
# into an object parameter as well.
class Owner
  def v = 5
end

class A
  def initialize(n = 1, x = 2.5, o = Owner.new)
    @n = n
    @x = x
    @o = o
  end

  def r = [@n, @x, @o.nil? ? :none : @o.v]
end

class B < A
  def initialize(n = 2, x = 1.5, o = Owner.new)
    super(n && n * 2, x, o)
  end
end

class Kw
  def initialize(n: 3) = (@n = n)
  def n = @n
end

class Maker
  def initialize = (@b = 1)
  def make(k) = k.new(@missing)
end

class Holder
  def initialize(o = Owner.new) = (@o = o)
  def r = @o.nil? ? :none : @o.v
end

class SubHolder < Holder
end

ks = [A, B]
p ks[ARGV.size].new(nil).r, ks[ARGV.size + 1].new(nil, nil, nil).r
p ks[ARGV.size].new(4, nil).r
kws = [Kw]
p kws[ARGV.size].new(n: nil).n, kws[ARGV.size].new.n
p Maker.new.make(ks[ARGV.size]).r, Maker.new.make(ks[ARGV.size + 1]).r
hs = [Holder, SubHolder]
p Maker.new.make(hs[ARGV.size]).r, Maker.new.make(hs[ARGV.size + 1]).r, hs[ARGV.size].new.r
