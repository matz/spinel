# A constructor's allocation takes its slot from the run of the thread it is
# on. The main thread and four more construct objects of three sizes at once,
# each holding its latest in a ring and checking an object as it leaves, and
# the main thread collects on the way: every object holds the values its own
# thread gave it, and a field no constructor set reads nil.

class Pair
  attr_reader :a, :b
  def initialize(a, b)
    @a = a
    @b = b
  end
end

class Wide
  attr_reader :a, :b, :c, :d, :e, :f, :g
  def initialize(a, v)
    @a = a
    @b = v
    @c = v + 1
    @d = v + 2
    @e = v + 3
    @f = v + 4
    @g = v + 5
  end

  def sum
    @b + @c + @d + @e + @f + @g
  end
end

class Slot
  attr_reader :n
  attr_accessor :items
  def initialize(n)
    @n = n
  end
end

RING = 64

def work(id, rounds)
  pairs = Array.new(RING) { |i| Pair.new(id, i - RING) }
  wides = Array.new(RING) { |i| Wide.new(id, i - RING) }
  slots = Array.new(RING) { |i| Slot.new(i - RING) }
  wrong = 0
  unset = 0
  rounds.times do |i|
    k = i % RING
    old = pairs[k]
    wrong += 1 if old.a != id || old.b != i - RING
    pairs[k] = Pair.new(id, i)
    w = wides[k]
    wrong += 1 if w.a != id || w.sum != (i - RING) * 6 + 15
    wides[k] = Wide.new(id, i)
    s = slots[k]
    wrong += 1 if s.n != i - RING
    t = Slot.new(i)
    unset += 1 if t.items.nil?
    slots[k] = t
    Thread.pass if i % 64 == 0
    GC.start if id == 0 && i % 1000 == 0
  end
  [wrong, unset]
end

threads = [1, 2, 3, 4].map { |id| Thread.new { work(id, 4000) } }
p work(0, 4000)
threads.each { |t| p t.value }
