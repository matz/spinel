# A table of Integer rows that moves its own rows around -- a heap's sift
# (`@heap[i] = @heap[parent]`, `last = @heap[n]; @heap[i] = last`) -- holds
# only the rows stored into it, so the moves say nothing of what the rows
# are. Counted as stores, they made the answer depend on the order of
# inference: a signature on push typed the moved rows boxed before the
# rows were known, and the boxed reads kept the heap boxed. infer-test
# greps the C: Heap's @heap is an sp_PtrArray * of Integer rows, and so is
# Plain's, the same heap with no signature.
class Heap
  def initialize
    @heap = []
    @size = 0
  end

  #: (Integer, Integer) -> void
  def push(vertex, priority)
    i = @size
    @size += 1
    if i >= @heap.size then @heap << [priority, vertex] else @heap[i] = [priority, vertex] end
    while i > 0
      parent = (i - 1) / 2
      break if @heap[parent][0] <= priority
      @heap[i] = @heap[parent]
      i = parent
    end
    @heap[i] = [priority, vertex]
  end

  def pop
    min = @heap[0]
    @size -= 1
    if @size > 0
      last = @heap[@size]
      i = 0
      while true
        l = 2 * i + 1
        r = l + 1
        s = i
        s = l if l < @size && @heap[l][0] < @heap[s][0]
        s = r if r < @size && @heap[r][0] < @heap[s][0]
        break if s == i
        @heap[i] = @heap[s]
        i = s
      end
      @heap[i] = last
    end
    min
  end

  def empty? = @size == 0
end

class Plain
  def initialize
    @heap = []
    @size = 0
  end

  def push(vertex, priority)
    i = @size
    @size += 1
    if i >= @heap.size then @heap << [priority, vertex] else @heap[i] = [priority, vertex] end
    while i > 0
      parent = (i - 1) / 2
      break if @heap[parent][0] <= priority
      @heap[i] = @heap[parent]
      i = parent
    end
    @heap[i] = [priority, vertex]
  end

  def pop
    min = @heap[0]
    @size -= 1
    if @size > 0
      last = @heap[@size]
      i = 0
      while true
        l = 2 * i + 1
        r = l + 1
        s = i
        s = l if l < @size && @heap[l][0] < @heap[s][0]
        s = r if r < @size && @heap[r][0] < @heap[s][0]
        break if s == i
        @heap[i] = @heap[s]
        i = s
      end
      @heap[i] = last
    end
    min
  end

  def empty? = @size == 0
end

h = Heap.new
[5, 3, 8, 1, 9, 2].each_with_index { |p, v| h.push(v, p) }
out = []
until h.empty?
  prio, val = h.pop
  out << prio * 10 + val
end
p out
h = Plain.new
[5, 3, 8, 1, 9, 2].each_with_index { |p, v| h.push(v, p) }
out = []
until h.empty?
  prio, val = h.pop
  out << prio * 10 + val
end
p out
