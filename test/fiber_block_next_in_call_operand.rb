# A `next` in the receiver, an argument or the `&blk` of a call that has a
# block, or in the collection of a `for`, is evaluated in the block the call
# is written in: in a Fiber.new, Thread.new or Enumerator.new block it leaves
# that block. The walk that says which `next` such a body owns stopped at a
# call with a block, and at a `for`, before it looked at them, so the `next`
# came out as a `continue` with no C loop around it, or, in a proc or under
# an `ensure`, built and answered nil.
c = ARGV.length == 0

# the receiver
p Fiber.new { r = (next 7 if c; [1, 2]).each { |v| v }; r }.resume
p Fiber.new { r = (next 7 unless c; [1, 2]).map { |v| v + 1 }; r }.resume
p Thread.new { r = (next 3 if c; [1, 2]).map { |v| v + 1 }; r }.value
e = Enumerator.new do |y|
  y << 1
  (next if c; [5, 6]).each { |v| y << v }
  y << 9
end
p e.to_a

# an argument
p Fiber.new { r = [1, 2].each_with_object((next 7 if c; [0])) { |v, a| a << v }; r }.resume
p Fiber.new { r = [1, 2].each_with_object((next 7 unless c; [0])) { |v, a| a << v }; r }.resume
p Thread.new { r = [3, 4].inject((next 8 if c; 10)) { |s, v| s + v }; r }.value
p Thread.new { r = [3, 4].inject((next 8 unless c; 10)) { |s, v| s + v }; r }.value

# a `&blk`, and the receiver and an argument of a call that is given one
inc = proc { |v| v + 1 }
p Fiber.new { r = [1, 2].map(&(next 7 if c; inc)); r }.resume
p Fiber.new { r = [1, 2].map(&(next 7 unless c; inc)); r }.resume
p Fiber.new { r = (next 6 if c; [1, 2]).map(&inc); r }.resume
p Thread.new { r = [3, 4].inject((next 8 if c; 10), &:+); r }.value

# two calls deep, and in an argument of a call in the receiver
p Fiber.new { r = (next 7 if c; [1, 2, 3]).map { |v| v + 1 }.select { |v| v > 2 }; r }.resume
p Fiber.new { r = (next 7 unless c; [1, 2, 3]).map { |v| v + 1 }.select { |v| v > 2 }; r }.resume
p Thread.new { r = [1, 2].zip((next 3 if c; [5, 6])).map { |a, b| a + b }; r }.value

# the collection of a for; a next in the for's body is the loop's
e = Enumerator.new do |y|
  y << 0
  for v in (next if c; [1, 2])
    y << v
  end
  y << 9
end
p e.to_a
t = Thread.new do
  s = 0
  for v in [1, 2, 3]
    next if v == 2
    s += v
  end
  for w in (next s if c; [10])
    s += w
  end
  s
end
p t.value
f = Fiber.new do
  s = 0
  for v in (next 7 unless c; [1, 2])
    for w in (next if v == 1; [5])
      s += w
    end
    s += v
  end
  s
end
p f.resume

# an iterator's block inside the body keeps the next in the receiver of a
# call written in it
e = Enumerator.new do |y|
  [1, 2, 3].each { |x| (next if x == 2; [x]).each { |v| y << v }; y << :after }
  y << :end
end
p e.to_a
f = Fiber.new do
  a = [1, 2, 3].map { |x| r = (next 0 if x == 2; [x]).map { |v| v * 10 }; r }
  next a if c
  :no
end
p f.resume

# in a proc the fiber's next is still the fiber's, and a proc written in the
# fiber keeps its own
pr = proc { Fiber.new { r = (next 5 if c; [1, 2]).map { |v| v + 1 }; r } }
p pr.call.resume
f = Fiber.new do
  q = proc { |x| r = (next x if c; [x]).map { |v| v + 1 }; r }
  [q.call(4), :outer]
end
p f.resume

# an ensure the next leaves runs first
f = Fiber.new do
  begin
    r = (next 6 if c; [1, 2]).each { |v| v }
    r
  ensure
    puts "ensure"
  end
end
p f.resume
e = Enumerator.new do |y|
  y << 1
  begin
    [2, 3].each_with_object((next if c; [0])) { |v, a| y << v }
  ensure
    puts "generator ensure"
  end
  y << 9
end
p e.to_a
