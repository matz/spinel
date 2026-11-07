# recv.method(:m) on a boxed value raises NameError when the value's class
# has no method m, as CRuby does at the lookup. A name the value has binds
# and calls as before. (An object of a program class is left as it was.)
class Plain
  def own = :own
end

def try_bind(v, tag)
  m = v.method(:zz)
  puts "#{tag}: bound #{m.name}"
rescue NameError => e
  puts "#{tag}: #{e.class} #{e.message} #{e.name.inspect}"
end

[nil, 1, 1.5, "s", [1], {a: 1}, true, false].each_with_index do |v, i|
  try_bind(v, i)
end

x = [nil, 1].first
p((x.method(:succ) rescue [$!.class, $!.message]))
y = [nil, 1].last
p y.method(:succ).call
p y.method(:+).call(4)
s = [nil, "ab"].last
p s.method(:upcase).call
p((s.method(:nope) rescue $!.message))

o = [Plain.new, 1].first
p o.method(:own).call
p o.method(:frozen?).call

# respond_to? on a boxed Integer or Float knows +@ and quo
[1, 2.5, nil].each { |v| p [v.respond_to?(:+@), v.respond_to?(:quo)] }
