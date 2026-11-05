# A Float parameter that can hold nil, as the receiver of % and **: those two
# alone refused it at compile time, where every other Float operator unwraps
# it and a nil raises NoMethodError.
def r(x)
  (x % 1).to_s
end

def pw(x)
  x ** 2
end

puts r(1.5)
f = ARGV.size > 3 ? nil : 2.25
puts r(f)
puts pw(1.5)
puts pw(f)
g = ARGV.size > 3 ? 2.0 : nil
begin
  r(g)
rescue NoMethodError => e
  puts e.message
end
begin
  pw(g)
rescue NoMethodError => e
  puts e.message
end
