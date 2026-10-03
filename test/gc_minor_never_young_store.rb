# A store of nil or of a frozen string literal takes no write barrier, and the
# stores beside it keep theirs.
#
# Such a store cannot make an old object point at a young one: nil is no
# object and the literal is static storage. A constructor is mostly these
# (`@left = nil`, `@tag = "n"`), so they are where the barrier's cost shows
# (gcbench runs three per Node). Every program here keeps a store that DOES
# need the barrier next to ones that do not, on a holder that is old by the
# time it runs, so the gc-minor-test leg under SPINEL_GC_VERIFY_GEN=1
# SPINEL_GC_STRESS=1 reports a barrier dropped from the wrong store:
#
#   * a node built with nil and literal fields, promoted, then given a young
#     child
#   * literals whose bytes look like the end of one in the emitted C (a NUL, a
#     quote, a backslash, `})`), since the pass decides on that text, and one
#     that spells a store, which must come back as it was written
#   * an initialize that allocates before and after its nil stores, and one
#     that stores what it allocated
#   * initialize called again on an object that is already old
#   * a field read into an argument, then replaced inside the callee

class Node
  attr_accessor :left, :right, :tag, :n
  def initialize(n)
    @left = nil
    @right = nil
    @tag = "n"
    @n = n
  end
end

def grow(depth)
  root = Node.new(depth)
  return root if depth == 0
  # root has been through an allocation, so under stress it is old here
  root.left = grow(depth - 1)
  root.right = grow(depth - 1)
  root.tag = "d" + depth.to_s
  root
end

def weigh(node)
  return 0 if node.nil?
  node.n + node.tag.length + weigh(node.left) + weigh(node.right)
end

class Label
  attr_reader :text, :plain, :odd, :spelt
  def initialize
    @plain = "plain"
    @text = "a\0b"
    @odd = "q\"}); x\\"
    @spelt = "self->iv_text = lv_s; self->iv_odd = NULL;"
  end
  def relabel
    @text = nil
    @text = "c\0d"
    @odd = @odd + "\" }; _fzl_9.d; })"
    @plain = "again"
  end
end

class Pair
  attr_reader :head, :tail, :note
  def initialize(n)
    @note = nil
    pad = [n, n + 1, n + 2]
    @head = nil
    @tail = [pad.length, n]
    @head = "h" + n.to_s
    @note = "lit"
  end
  def again(n)
    initialize(n)
    self
  end
end

class Box
  attr_accessor :item
  def initialize
    @item = [1, 2, 3]
  end
end

def swap_and_sum(box, old)
  box.item = [10, 20, 30, 40]
  filler = Array.new(8) { |i| i.to_s }
  old.sum + box.item.sum + filler.length
end

t = grow(6)
puts weigh(t)

labels = []
5.times { labels << Label.new }
labels.each { |l| l.relabel }
puts labels.map { |l| l.text.bytes.sum + l.plain.length + l.odd.length }.sum
puts labels[0].spelt

pairs = []
20.times { |i| pairs << Pair.new(i) }
pairs.each_with_index { |p, i| p.again(i + 100) }
puts pairs.map { |p| p.head.length + p.tail.sum + p.note.length }.sum

b = Box.new
puts swap_and_sum(b, b.item)
puts b.item.length
