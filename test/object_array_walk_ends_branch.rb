# A walk over an Array of a class's objects that ends a branch of an if,
# unless or case is that conditional's value. Where the conditional's own
# value is thrown away -- a statement followed by another, a top-level
# statement, a loop body's last, an unread method's last -- the walk's is
# too, so the Array narrows (`if c; list.each { }; end; nil`). Where the
# conditional's value is read, the walk answers the Array as before.
# infer-test greps the C: each class's @items is an sp_PtrArray *, but
# Shelf's, whose walk's value is read, stays an sp_PolyArray *.
class Item
  attr_reader :n
  def initialize(n) = @n = n
end

class Bag
  def initialize(k) = @items = Array.new(k) { |i| Item.new(i + 1) }

  def if_stmt(t)
    if t > 0
      @items.each { |a| t += a.n }
    end
    t
  end
end

class Box
  def initialize(k) = @items = Array.new(k) { |i| Item.new(i * 2) }

  def branches(m)
    s = 0
    if m == 0
      @items.each { |b| s += b.n }
    elsif m == 1
      @items.reverse_each { |c| s -= c.n }
    else
      @items.each_with_index { |d, i| s += d.n * i }
    end
    unless m > 5
      @items.each { |e| s += 1 if e.n > 2 }
    end
    case m
    when 0 then @items.each { |f| s += f.n * 10 }
    when 1 then nil
    else @items.each { |g| s -= g.n }
    end
    m > 1 ? @items.each { |h| s += h.n } : nil
    s
  end
end

class Crate
  def initialize(k) = @items = Array.new(k) { |i| Item.new(i + 3) }

  def nested(x)
    total = 0
    if x > 0
      if x > 1
        @items.each { |j| total += j.n }
      end
    end
    total
  end

  def in_loop(x)
    total = 0
    x.times do
      @items.each { |w| total += w.n } if x.odd?
    end
    total
  end
end

# an unread method whose last statement is the conditional
class Tray
  def initialize(k) = @items = Array.new(k) { |i| Item.new(i + 5) }

  def tail(x)
    @sum = 0
    if x
      @items.each { |r| @sum += r.n }
    end
  end

  def sum = @sum
end

# the conditional's value is read: the walk answers the Array
class Shelf
  def initialize(k) = @items = Array.new(k) { |i| Item.new(i + 7) }

  def answer(x)
    if x
      @items.each { |q| q.n }
    else
      []
    end
  end
end

p Bag.new(4).if_stmt(1)
p Bag.new(4).if_stmt(0)
b = Box.new(4)
p [0, 1, 2, 7].map { |m| b.branches(m) }
cr = Crate.new(3)
p [cr.nested(2), cr.nested(1), cr.in_loop(3), cr.in_loop(2)]
t = Tray.new(3)
t.tail(true)
p t.sum
v = Shelf.new(2).answer(true)
p v.map(&:n)
p Shelf.new(2).answer(false)
