# A `next` in the block of Fiber.new, Thread.new or Enumerator.new leaves the
# block: the fiber or thread ends with the `next`'s value (nil without one),
# and the generator stops yielding. The block is a C function of its own with
# no loop around it, and the `next` came out as a `continue`, which no C
# compiler accepts there.
c = ARGV.length == 0

# with a value, without one, with several
p Fiber.new { next 5 if c; 7 }.resume
p Fiber.new { next if c; 7 }.resume
p Fiber.new { next 1, "b" if c; 7 }.resume
p Fiber.new { next 5 unless c; 7 }.resume
p Thread.new { next 3 if c; 4 }.value
p Thread.new(10) { |n| next n + 1 if c; 4 }.value
p (1..4).map { |i| Thread.new(i) { |n| next :odd if n.odd?; n * 2 } }.map(&:value)

# a generator: the yields before the next are kept, the ones after are not
# made, and the value is StopIteration#result
e = Enumerator.new do |y|
  y << 1
  y.yield 2
  next :done if c
  y << 3
end
p e.to_a, e.first(1), e.map { |x| x * 10 }
e.next
e.next
begin
  e.next
rescue StopIteration => ex
  p ex.result
end

# nested in if/else and case, and as the block's last statement
f = Fiber.new do |x|
  if x > 1
    if c
      next x * 2
    end
    puts "not reached"
  else
    next :small unless x > 5
  end
  :tail
end
p f.resume(3)
p Fiber.new { |x| if x > 1 then :big else next :small end }.resume(0)
3.times do |i|
  g = Fiber.new do |x|
    case x
    when 0 then next :zero
    when 1 then next
    end
    :other
  end
  p g.resume(i)
end
p Fiber.new { puts "a"; next 8 }.resume

# after a Fiber.yield the next ends the fiber
f = Fiber.new do
  Fiber.yield 1
  next 2 if c
  Fiber.yield 3
end
p f.resume, f.resume, f.alive?

# a while loop or an iterator's block inside the body keeps its own next
f = Fiber.new do
  i = 0
  s = 0
  while i < 5
    i += 1
    next if i.odd?
    s += i
  end
  a = [1, 2, 3].map { |x| next 0 if x == 2; x }
  [1, 2, 3].each { |x| next if x == 1; s += x }
  next [s, a] if c
  :no
end
p f.resume
e = Enumerator.new do |y|
  [1, 2, 3].each { |x| next if x == 2; y << x }
  i = 0
  until i == 4
    i += 1
    next if i.even?
    y << i * 10
  end
  next if c
  y << 99
end
p e.to_a

# an ensure the next leaves runs first, inner before outer, and a rescue it
# leaves is no longer the handler afterwards
f = Fiber.new do
  begin
    begin
      next 6 if c
    ensure
      puts "inner"
    end
    puts "not reached"
  ensure
    puts "outer"
  end
  8
end
p f.resume
t = Thread.new do
  begin
    next :t if c
    :no
  rescue => err
    :rescued
  end
end
p t.value
f = Fiber.new do
  begin
    raise "boom"
  rescue => err
    begin
      next err.message if c
    ensure
      puts "in rescue"
    end
  end
  12
end
p f.resume
begin
  raise "after"
rescue => err
  puts err.message
end
e = Enumerator.new do |y|
  begin
    y << 1
    next if c
    y << 2
  ensure
    puts "generator ensure"
  end
end
p e.to_a

# each body's next is its own: a proc or lambda inside a fiber, a fiber
# inside a proc, a fiber inside a fiber, and bodies written in a method
f = Fiber.new do
  pr = proc { |x| next x + 1 if c; 0 }
  l = ->(x) { next x * 2 if c; 0 }
  next [pr.call(1), l.call(2)] if c
  :no
end
p f.resume
pr = proc { Fiber.new { next 5 if c; 7 } }
p pr.call.resume
f = Fiber.new do
  g = Fiber.new { next :inner if c; :x }
  v = g.resume
  next [v, :outer] if c
  :y
end
p f.resume
class Holder
  def initialize
    @v = 4
  end

  def run(c)
    [Fiber.new { next @v if c; @v + 1 }.resume, Thread.new { next @v * 2 if c; 0 }.value]
  end

  def gen(c) = Enumerator.new { |y| y << @v; next if c; y << 0 }
end
h = Holder.new
p h.run(true), h.run(false), h.gen(true).to_a, h.gen(false).to_a

# a captured local written before the next keeps the write
x = 0
f = Fiber.new { x = 1; next x + 1 if c; x = 9 }
p f.resume, x
