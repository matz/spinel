# Test block_given? and optional blocks

def maybe_yield(x)
  if block_given?
    yield x
  else
    x * 2
  end
end

puts maybe_yield(5)  # 10 (no block)

result = 0
maybe_yield(5) do |n|
  result = n * 3
end
puts result  # 15

# Method that always yields
def each_pair(a, b)
  yield a
  yield b
end

total = 0
each_pair(10, 20) do |n|
  total += n
end
puts total  # 30

puts "done"
