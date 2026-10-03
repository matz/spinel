# A `next` written as an expression in the receiver or an argument of a call
# that takes a block leaves the Fiber, Thread or Enumerator block: the
# receiver and the arguments are evaluated in the body, not in the call's
# block. The call was made with the `next`'s value in its place.
$c = true

f1 = Fiber.new do
  r = ((next 7 if $c); [1, 2]).map { |v| v + 1 }
  r
end
p f1.resume
f2 = Fiber.new do
  r = [3, 4].inject(((next 8 if $c); 10)) { |s, v| s + v }
  r
end
p f2.resume
t1 = Thread.new do
  r = ((next 3 if $c); [1, 2]).map { |v| v * 2 }
  r
end
p t1.value
e1 = Enumerator.new do |y|
  y << 1
  ((next if $c); [5, 6]).each { |v| y << v }
  y << 9
end
p e1.to_a

# the call's own block keeps its `next`
f3 = Fiber.new do
  r = ((next 7 unless $c); [1, 2, 3]).map { |v| next 0 if v == 2; v + 1 }
  r
end
p f3.resume

# not taken: the call is made
$c = false
f4 = Fiber.new do
  r = ((next 7 if $c); [1, 2]).map { |v| v + 1 }
  r
end
p f4.resume
f5 = Fiber.new do
  r = [3, 4].inject(((next 8 if $c); 10)) { |s, v| s + v }
  r
end
p f5.resume
t2 = Thread.new do
  r = ((next 3 if $c); [1, 2]).map { |v| v * 2 }
  r
end
p t2.value
e2 = Enumerator.new do |y|
  y << 1
  ((next if $c); [5, 6]).each { |v| y << v }
  y << 9
end
p e2.to_a
