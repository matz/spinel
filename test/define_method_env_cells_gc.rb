# spinel: gc-stress
# The environment of a define_method block, with the cells it holds, is
# reachable only through the definer's global and the running method's frame:
# collections between runs and inside the defined method keep the String, the
# Array, the Hash and the counter that the block closes over.
class Held
  def self.mk(n)
    s = +"seed"
    arr = []
    seen = {}
    count = 0
    define_method(:m) do |x|
      count += 1
      arr << (s + x.to_s * n.to_s.size)
      seen[x] = arr.last.size
      GC.start
      [count, arr.size, seen.size]
    end
    define_method(:count) { count }
    define_singleton_method(:peek) { [s, arr.size, seen.size] }
    :ok
  end
end
Held.mk(12)
GC.start
k = Held.new
200.times { |i| k.m(i) }
GC.start
p k.m(1), k.count, Held.peek
Held.mk(7)
GC.start
p k.count
p k.m(2), Held.peek
junk = Array.new(2000) { |i| "junk#{i}" * 3 }
p k.m(3), junk.size, Held.peek

# a default that runs the definer again, and collects, keeps the environment it was bound to
class Rerun
  def self.mk(n)
    define_method(:m) { |a = (Rerun.mk(n); GC.start; n)| [a, n] }
    :ok
  end
end
Rerun.mk(1)
p Rerun.new.m
p Rerun.new.m

# a default that runs program code with no call in its tree (a case's `===`, an or-write through a reader) can run
# the definer again too: the environment stays rooted
class Trig
  def self.===(o)
    Whn.mk(o)
    GC.start
    true
  end
end
class Whn
  def self.mk(n)
    s = "s#{n}"
    define_method(:m) { |a = (case n when Trig then s end)| a }
    :ok
  end
end
Whn.mk(1)
p Whn.new.m
class Box
  def v
    Orw.mk(2)
    GC.start
    nil
  end
  attr_writer :v
end
class Orw
  def self.mk(n)
    s = "s#{n}"
    o = Box.new
    define_method(:m) { |a = ((o.v ||= 9); s)| a }
    :ok
  end
end
Orw.mk(2)
p Orw.new.m
