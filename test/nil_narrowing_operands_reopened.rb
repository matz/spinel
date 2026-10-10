# A program whose Float#< takes a nil: `x < y` on a nil y answers rather
# than raising, so y is not narrowed by it and the next comparison still
# raises.
class Float
  alias_method :old_lt, :<
  def <(other) = other.nil? ? true : old_lt(other)
end

def check(a, i, x)
  y = a[i]
  r = x < y
  s = y > 0.0
  [r, s]
end

def try
  yield
rescue NoMethodError => e
  puts "#{e.class}: #{e.message}"
end

p check([5.0], 0, 1.0)
try { p check([5.0], 3, 1.0) }
