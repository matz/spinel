# A builtin `new` the call plan refuses (an empty initialize prepended ahead of a forward that checks a `**h`'s keys)
# is refused where it is a rescue modifier's or a begin's value, as where it runs for its effect alone.
module E; def initialize(*) = nil; end
module A; def initialize(*a, capacity: 1) = super; end
Hash.prepend(E); Hash.prepend(A)
h = { bar: 1 }
p((Hash.new(**h) rescue $!.message))
x = begin
  Hash.new(**h)
rescue ArgumentError => e
  e.message
end
p x
