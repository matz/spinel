# try_convert on a class read out of a mixed container: the value when it
# already is one of that class (a Float through to_int for Integer), nil
# otherwise, as the constant receiver answers; NoMethodError for a boxed
# Integer and for nil, through a method parameter that is a class elsewhere.
a = [Array, 0][0]
p a.try_convert([1, 2])
p a.try_convert("x")
p a.try_convert(nil)
h = [Hash, 0][0]
p h.try_convert({a: 1})
p h.try_convert([[1, 2]])
s = [String, 0][0]
p s.try_convert("x")
p s.try_convert(:x)
buf = +"m"
buf << "n"
p s.try_convert(buf)
i = [Integer, 0][0]
p i.try_convert(7)
p i.try_convert(2**70)
p i.try_convert(1.5)
p i.try_convert("7")
r = [Regexp, 0][0]
p r.try_convert(/x/)
p r.try_convert("x")
io = [IO, 0][0]
p io.try_convert($stdout).class
p io.try_convert("out")
v = [1, "x"][1]
p s.try_convert(v)
p a.try_convert(v)
def conv(k, x) = k.try_convert(x)
p conv([Integer, 0][0], 3)
p conv([String, 0][0], 3)
p s.try_convert("ab").upcase
p a.try_convert([1, 2]).size
p [[Array, [1]], [Hash, [1]], [Integer, 4]].map { |k, x| k.try_convert(x) }
[[3, 0][0], [nil, 0][0]].each do |k|
  begin
    conv(k, 1)
  rescue NoMethodError => e
    puts e.message
  end
end
