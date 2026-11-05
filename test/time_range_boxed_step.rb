# A boxed Range of Times walks by step with a block, beside an Integer Range
def walk(r, n)
  xs = []
  ret = r.step(n) { |x| xs << x }
  [xs, ret == r]
end

def first_past(r, n, lim)
  r.step(n) { |x| break x if x > lim }
end

t = Time.at(0).utc
r = t..(t + 7200)
xs, same = walk(r, 3600)
p xs.map(&:hour), same
p walk(1..5, 2)
xs, = walk(t...(t + 7200), 3600)
p xs.map(&:hour)
p first_past(t.., 1800, t + 4000).min
p first_past(1..10, 3, 5)
begin
  walk(r, 0)
rescue ArgumentError => e
  p e.message
end
begin
  walk(:sym, 1)
rescue NoMethodError => e
  p e.message
end
