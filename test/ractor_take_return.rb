# Ractor block return value is delivered to the first r.take (#1453), matching
# CRuby: a Ractor whose block simply computes a value (no explicit Ractor.yield)
# still hands that value back through take.
r = Ractor.new do
  21 * 2
end
puts r.take

# A string result crosses the deep-copy boundary as the block value.
s = Ractor.new do
  "hello" + " world"
end
puts s.take

# Spawn arg + computed return value (no explicit yield).
t = Ractor.new(100) do |n|
  n + 23
end
puts t.take

# An explicit Ractor.yield is delivered first; the block's return value follows
# as a second take (FIFO order preserved).
u = Ractor.new do
  Ractor.yield(1)
  2
end
puts u.take
puts u.take
