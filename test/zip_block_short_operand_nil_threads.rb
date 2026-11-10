# spinel: gc-stress
# The zip block's argument nil past a shorter operand (see
# zip_block_short_operand_nil.rb) in a program with threads, whose walk
# polls at each pass and reads the operand through the header it holds,
# read again after each poll.

def fdot(a, b)
  s = 0.0
  a.zip(b) { |x, y| s += x * y }
  s
end

def idot(a, b)
  s = 0
  a.zip(b) { |x, y| s += x * y }
  s
end

def try
  yield
rescue TypeError => e
  "#{e.class}: #{e.message}"
end

long = Array.new(2000) { |i| (i % 9) * 0.5 }
ilong = Array.new(2000) { |i| i % 7 }
workers = Array.new(4) do |w|
  Thread.new do
    acc = 0.0
    iacc = 0
    20.times do |k|
      acc += fdot(long, long)
      iacc += idot(ilong, ilong)
    end
    [acc, iacc, try { fdot(long, long.first(1999 - w)) }, try { idot(ilong, ilong.first(10)) }]
  end
end
workers.each { |t| p t.value }

def matmul(a, t)
  a.map { |ai| t.map { |tj| s = 0.0; ai.zip(tj) { |x, y| s += x * y }; s } }
end
b = Array.new(3) { |i| Array.new(3) { |j| (i + j) * 0.25 } }
t = Array.new(3) { |j| Array.new(3) { |i| b[i][j] } }
p Thread.new { matmul(b, t) }.value
ragged = [[1.0, 2.0], [3.0]]
rt = Array.new(2) { |j| Array.new(2) { |i| ragged[i][j] } }
p Thread.new { try { matmul(Array.new(1) { [1.0, 1.0] }, rt) } }.value

def fill(a, b, d)
  out = []
  a.zip(b) { |x, y| out << (y.nil? ? d : x + y) }
  out
end
p fill([1.0, 2.0, 3.0], [0.5], -1.0)
p fill([1, 2, 3], [10, 20], 0)
