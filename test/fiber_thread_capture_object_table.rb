# spinel: gc-stress
# A Fiber or Thread block that captures a local Array of one class's objects
# keeps it alive. The block's captures live in a struct the collector scans,
# which marked every heap-backed capture but this table, so once the method
# that made the Fiber returned, a collection freed the table and the block
# read the objects allocated over it.

class Item
  attr_reader :i
  def initialize(i) = @i = i
end

def fiber_over_items
  items = Array.new(3) { |i| Item.new(i) }
  Fiber.new do
    k = 0
    while k < items.size
      Fiber.yield items[k].i
      k += 1
    end
    nil
  end
end

def thread_over_items(q)
  items = Array.new(3) { |i| Item.new(i + 10) }
  Thread.new do
    q.pop
    s = 0
    k = 0
    while k < items.size
      s += items[k].i
      k += 1
    end
    s
  end
end

def churn
  junk = []
  50_000.times { |j| junk << Item.new(j + 100) }
  junk = nil
  GC.start
  50_000.times { |j| Item.new(j + 100) }
end

f = fiber_over_items
q = Queue.new
t = thread_over_items(q)
churn
p f.resume, f.resume, f.resume
q << 1
p t.value
