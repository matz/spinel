# Queue.new(items) starts the Queue with the elements of items, converted
# with to_a: an Array, a Range, a Hash's pairs, or nil for none
q = Queue.new([1, 2, 3])
p q.size
p q.empty?
p q.pop
p q.pop(timeout: 0)

r = Thread::Queue.new(4..6)
p [r.pop, r.pop, r.size]

h = Queue.new({a: 1})
p h.pop

n = Queue.new(nil)
p n.size

mixed = Queue.new(["s", :t, 2.5])
p [mixed.pop, mixed.pop, mixed.pop]

calls = 0
items = -> { calls += 1; [7] }
e = Queue.new(items.call)
p [e.pop, calls]

c = Queue.new([1])
c << 2
c.close
p [c.pop, c.pop, c.pop]

class Bag
  def to_a = [7, 8]
end
b = Queue.new(Bag.new)
p [b.pop, b.pop, b.size]
