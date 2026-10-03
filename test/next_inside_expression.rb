# A `next` inside an expression leaves the block from there: the value of an
# assignment, an operand, an argument, an element, `c && (next)` ahead of
# more statements. It was taken as its own value, so the expression went on
# with it and the block ran to its end.
c = ARGV.length == 0

def twice(x) = x * 2

# the value of an assignment: a ternary, an if, a case
p [1, 2, 3].map { |v| x = (v == 2 ? (next 0) : v); x * 10 }
r = [1, 2, 3].map do |v|
  x = if v == 2
    next "two"
  else
    v
  end
  x.to_s + "!"
end
p r
r = [1, 2, 3].map do |v|
  x = case v
      when 2 then next :two
      else v
      end
  [x]
end
p r
p [1, 2, 3].map { |v| y = (next v if v > 1); y.inspect }

# an argument, an interpolation, an element, an operand
p [1, 2, 3].map { |v| twice(v == 1 ? (next 9) : v) }
p [1, 2, 3].map { |v| "v#{v == 2 ? (next "skip") : v}" }
p [1, 2, 3].map { |v| [v, (next -1 if v == 3), v] }
r = %w[a b c].each_with_index.map do |s, i|
  t = s + (i == 1 ? (next s.upcase) : "-")
  t * 2
end
p r
r = [1, 2, 3].map do |v|
  s = "s#{v}"
  a = [s, (next s + "!" if v == 2), s * 2]
  a.join(",")
end
p r

# `&&` and `||` with the next in parentheses, ahead of more statements
r = [1, 2].map do |x|
  c && (next 4)
  5
end
p r
[1, 2, 3].each do |v|
  v == 2 && (next)
  puts v
end
[1, 2, 3].each do |v|
  v != 2 || (next)
  puts v
end
[1, 2, 3].each do |v|
  (v == 2) && (puts "two"; next)
  puts v
end
3.times do |i|
  i == 1 && (next)
  puts i
end

# the blocks of other iterators
p [1, 2, 3, 4].select { |v| y = (v == 2 ? (next false) : true); y }
p [1, 2, 3, 4].reject { |v| y = (v == 2 ? (next true) : false); y }
p [1, 2, 3, 4].inject(0) { |a, v| w = (v == 3 ? (next a) : v); a + w }
p [1, 2, 3].each_with_index.map { |v, i| z = (i == 1 ? (next :mid) : v); z }
p [3, 1, 2].sort_by { |v| k = (v == 1 ? (next 9) : v); k }
p [1, 2, 3].filter_map { |v| w = (v == 2 ? (next) : v); w * 2 }
acc = [1, 2, 3].each_with_object([]) do |v, memo|
  w = (v == 2 ? (next) : v)
  memo << "w#{w}"
end
p acc
h = { a: 1, b: 2 }
h.each { |k, v| w = (v == 1 ? (next) : v); puts "#{k} #{w}" }

# a block inside a block: each next leaves its own
r = [1, 2].map do |a|
  inner = [10, 20].map { |b| x = (b == 10 ? (next 0) : b); x + a }
  y = (a == 2 ? (next inner.sum) : a)
  [y, inner]
end
p r

# while, until and for: the next iteration
i = 0
out = []
while i < 4
  i += 1
  x = (i == 2 ? (next) : i)
  out << x
end
p out
i = 0
while i < 3
  i += 1
  c && (next)
  puts "not reached"
end
i = 0
until i >= 4
  i += 1
  i.odd? && (next)
  puts i
end
for j in 1..4
  y = (j.even? ? (next) : j)
  puts y
end

# a proc, a lambda, a Fiber, a Thread, an Enumerator
pr = proc { x = (c ? (next 5) : 7); x + 1 }
p pr.call
l1 = ->(v) { x = (v ? (next 1) : 2); x + 1 }
p l1.call(true), l1.call(false)
l2 = lambda { |v| v && (next "yes"); "no" }
p l2.call(true), l2.call(false)
f = Fiber.new { x = (c ? (next 5) : 7); x + 1 }
p f.resume
f = Fiber.new { x = [1, (next 5 if c), 3]; x }
p f.resume
f = Fiber.new { c && (next :left); :stayed }
p f.resume
t = Thread.new { x = (c ? (next 3) : 4); x + 1 }
p t.value
e = Enumerator.new { |y| y << 1; c && (next); y << 2 }
p e.to_a

# through an ensure and out of a rescue
r = [1, 2, 3].map do |v|
  begin
    x = (v == 2 ? (next :two) : v)
    x * 10
  ensure
    puts "ensure #{v}"
  end
end
p r
pe = proc do |v|
  begin
    x = (v == 2 ? (next :two) : v)
    x * 10
  ensure
    puts "ensure #{v}"
  end
end
p pe.call(1), pe.call(2)
out = []
[1, 2, 3].each do |v|
  begin
    v == 2 && (next)
    out << v
  rescue
    out << :rescued
  end
end
p out

# where the block's value is written, a next is still its value
p [1, 2].map { |v| next v * 2 }
p [1, 2].map { |v| if v == 1 then next 5 else 6 end }
p [1, 2].map { |v| (next v + 1) }
p [1, 2].map { |v| begin; next v + 1; rescue; 0; end }
p [1, 2].map { |v| case v when 1 then next :one else :other end }
pt = proc { |v| next v * 3 }
p pt.call(2)
pq = proc { |v| v > 1 ? (next :big) : :small }
p pq.call(2), pq.call(1)
p Fiber.new { next 5 }.resume
p 5.instance_eval { next self + 1 }
p "ab".then { |s| next s * 2 }
