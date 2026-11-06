# `k.at(s)` on a Class value from a block parameter is Time's maker (a Time
# subclass makes its own instance), not Array#at
class Stamp < Time; end
[Time, Stamp].each { |k| p k.at(0).class }
[Time, Stamp].each { |k| t = k.at(0); p t.to_i }
x = [Stamp, Time].map { |k| k.at(5) }
p x.map(&:class), x.map(&:to_i)
def go(ks) = ks.map { |k| k.at(1).class }
p go([Time, Stamp])
# an Array element keeps Array#at
[[1, 2], [3, 4]].each { |k| p k.at(1) }
[[1, "a"], [nil, 2.5]].each { |k| p k.at(0) }
[[9]].each { |k| p k.at(-1) }
z = ["ab", [1]]
z.each { |k| begin; p k.at(0); rescue NoMethodError => e; p e.class; end }
# a Class that has no `at`: the argument runs before the NoMethodError
(ARGV.empty? ? Integer : Time).then do |k|
  begin; k.at((puts "arg"; 0)); rescue NoMethodError => e; p e.message; end
end
def mk(k) = k.at((puts "arg2"; 0))
begin; mk(Integer); rescue NoMethodError => e; p e.message; end
