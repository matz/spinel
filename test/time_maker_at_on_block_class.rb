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
# keyword values and a double splat run too, in order
def mkw(k) = k.at((puts "a1"; 0), in: (puts "kw"; "+00:00"), **(puts "dsplat"; {}))
begin; mkw(Integer); rescue NoMethodError => e; p e.message; end
# more than 16 arguments that run code
def m17(k) = k.at((print 1;0),(print 2;0),(print 3;0),(print 4;0),(print 5;0),(print 6;0),(print 7;0),(print 8;0),(print 9;0),(print 10;0),(print 11;0),(print 12;0),(print 13;0),(print 14;0),(print 15;0),(print 16;0),(puts 17;0))
begin; m17(Integer); rescue NoMethodError => e; p e.message; end
