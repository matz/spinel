# A method's closure cells are made by the statement that builds the
# closure: the captured locals keep their values and their sharing when the
# block sits in a branch, in a loop, past a write, beside a rescue, or in
# the arm of a poly receiver's #each that only some receivers take.
# The collector runs at every allocation in the gc legs: a cell made after
# the struct that takes it, or one made afresh for each run of a block, stays
# alive.
# spinel: gc-stress
# spinel: gc-minor

class Bag
  def initialize(items)
    @items = items
  end

  def each
    @items.each { |x| yield x }
  end
end

# a block only in one arm of a case
def pick(kind, items)
  out = []
  case kind
  when :list
    items.each { |x| out << x * 2 }
  when :none
    out << 0
  else
    out << -1
  end
  out
end

# a poly receiver: a Hash's #each is inlined, a Bag's takes a closure
def symbols(pairs)
  options = {}
  pairs.each { |name, value| options[name.to_s.to_sym] = value }
  options
end

# writes before the closure, in it and after it, and a second closure
def tally(n, xs)
  total = 0
  n += 1
  total += n
  xs.each { |x| total += x * n; n += 1 }
  n += 100
  xs.each { |x| total += x }
  [total, n]
end

# the closure made in a loop: every iteration shares the one cell
def loops(xs)
  seen = 0
  i = 0
  while i < 4
    xs.each { |x| seen += x + i } if i.odd?
    i += 1
  end
  seen
end

# a proc that outlives the call, and one made only on request
def counter(start, make)
  count = start
  count += 1
  return count unless make
  inc = proc { count += 1 }
  inc.call
  [inc, count]
end

# a `return` inside the block: the method's proc-return frame
def first_big(xs, limit)
  return :none if xs.nil?
  found = 0
  xs.each do |x|
    found += 1
    return [:found, x, found] if x > limit
  end
  [:small, found]
end

# a rescue in the method keeps the cells on entry
def guarded(xs, fail)
  acc = []
  begin
    xs.each { |x| acc << x }
    raise ArgumentError, "no" if fail
  rescue ArgumentError
    acc << :rescued
  end
  acc
end

# a block's local captured by a proc: a fresh cell each run of the block
def fresh(xs)
  made = []
  xs.each do |x|
    kept = []
    kept << x * 10
    made << proc { kept }
  end
  GC.start
  made.map { |pr| pr.call.first }
end

# a closure held only by a global, its cell made after the struct
$held = nil
def hold(name, n)
  label = name + "!"
  $held = proc { |k| label * (n + k) }
  :held
end

p pick(:list, Bag.new([1, 2]))
p pick(:list, [3, 4])
p pick(:none, Bag.new([1]))
p pick(:other, nil)
p symbols({ "a" => 1, "b" => 2 })
p symbols(Bag.new([["c", 3], ["d", 4]]))
p tally(2, [1, 2])
p tally(0, Bag.new([5]))
p tally(1, [])
p loops([1, 2])
p loops(Bag.new([10]))
p counter(5, false)
inc, now = counter(1, true)
p now
inc.call
p inc.call
p first_big(Bag.new([1, 9, 3]), 5)
p first_big([1, 2], 5)
p first_big(nil, 5)
p guarded([1, 2], false)
p guarded(Bag.new([3]), true)
p fresh([1, 2, 3])
p hold(+"ab", 1)
GC.start
junk = Array.new(500) { |i| "junk#{i}" }
p $held.call(1), junk.size
