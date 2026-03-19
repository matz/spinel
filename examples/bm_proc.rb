# Test &block, Proc.new, proc, Proc#call

# &block parameter — receive block as Proc
def apply(x, &block)
  block.call(x)
end

puts apply(5) { |n| n * 2 }  # 10

# Store block in variable
def make_doubler
  proc { |n| n * 2 }
end

doubler = make_doubler
puts doubler.call(7)  # 14

# Proc.new
adder = Proc.new { |n| n + 10 }
puts adder.call(5)  # 15

# Pass proc to method
def transform(x, fn)
  fn.call(x)
end

puts transform(3, doubler)  # 6

# Method with &block forwarding
def with_logging(x, &blk)
  puts "before"
  result = blk.call(x)
  puts "after"
  result
end

with_logging(42) do |n|
  puts n
end
# before, 42, after

puts "done"
