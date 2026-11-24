# A boxed String or Symbol's upcase, downcase, swapcase and capitalize
# take their options from a splat, checked as the other operands are

def opts(*o) = o

x = [1, "Iıi"][1]
a = [1, "dEf"][1]
p a.upcase(*opts(:ascii))
p x.upcase(*opts(:turkic))
p x.downcase(*opts(:turkic, :lithuanian))
p x.swapcase(*opts)
p a.capitalize(*opts(:ascii))

s = [1, :abc][1]
p s.upcase(*opts(:ascii))
p s.swapcase(*opts)

begin
  p x.upcase(*opts(:bogus))
rescue ArgumentError => e
  p [e.class, e.message]
end
begin
  p x.capitalize(*opts(:turkic, :ascii))
rescue ArgumentError => e
  p [e.class, e.message]
end

begin
  p a.upcase(*opts(:turkic, :lithuanian, :ascii))
rescue ArgumentError => e
  p e.message
end
begin
  p x.send(:downcase, *opts(:fold, :ascii, :turkic, :lithuanian))
rescue ArgumentError => e
  p e.message
end

n = [1, nil][1]
begin
  n.upcase(*opts(:ascii))
rescue NoMethodError => e
  p e.class
end

# two calls in one statement, and one through a rest parameter
def up(v, *o) = v.upcase(*o)
p [x.downcase(*opts), up(a, *opts(:ascii))]
p up(s)
