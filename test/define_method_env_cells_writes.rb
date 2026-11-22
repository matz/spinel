# The locals a define_method block closes over are the method's own for every form that reads or writes a local, not
# only a plain read: a multiple assignment, a pattern's binding and an operator write through an attribute of a
# captured object, each with nil, a boxed value, a String and a Float among the captured locals, and one run after
# another.
class Pt
  attr_accessor :v
  def initialize(v) = @v = v
end
class Masgn
  def self.mk(n)
    a = [n, n + 1]; z = nil; f = 1.5
    define_method(:swap) { x, y = a; n, f = y, x; [n, f] }
    define_method(:first) { n, = a; n }
    define_method(:rest) { q, *z = a; [q, z] }
    :ok
  end
end
Masgn.mk(3)
k = Masgn.new
p k.swap, k.swap, k.first, k.rest
Masgn.mk(5)
p k.swap, k.first
class Pat
  def self.mk(n)
    a = [n, n + 1]; q = 0; z = nil
    define_method(:pm) { case a
      in [q, z] then [q, z, n]
      end }
    :ok
  end
end
Pat.mk(3)
p Pat.new.pm
Pat.mk(5)
p Pat.new.pm
class Opw
  def self.mk(n)
    o = Pt.new(n)
    define_method(:bump) { o.v += 1 }
    define_method(:orw) { o.v ||= 9 }
    o
  end
end
o = Opw.mk(3)
p Opw.new.bump, Opw.new.bump, o.v, Opw.new.orw

# a multiple assignment with a splat and a post target writes the captured locals through their cells
class Splat
  def self.mk
    head, *middle, n = [1, 2, 3, 4]
    define_singleton_method(:rd) { [head, middle, n] }
    *init, last = [5, 6, 7]
    define_singleton_method(:rd2) { [init, last] }
    :ok
  end
end
Splat.mk
p Splat.rd, Splat.rd2
