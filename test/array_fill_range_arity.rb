# A Range is valid as the second argument only for fill(value, range).
# With a third argument, CRuby treats it as an integer start and raises.
p(([].fill("x", 0..2, 5) rescue [$!.class, $!.message]))
p(([].fill("x", 1.0..2.0, 1) rescue [$!.class, $!.message]))
p(([].fill("x", "a".."b", 1) rescue [$!.class, $!.message]))

def fill_receiver
  puts "receiver"
  []
end

def fill_value
  puts "value"
  "x"
end

def fill_range
  puts "range"
  0..2
end

def fill_length
  puts "length"
  1
end

p((fill_receiver.fill(fill_value, fill_range, fill_length) rescue [$!.class, $!.message]))
