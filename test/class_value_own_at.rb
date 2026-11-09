# A Class value whose class has its own self.at runs that method; a value
# that is an Array at run time still reads its element (Array#at).
class Stamp; def self.at(x) = "stamp#{x}"; end
module Mod; def self.at(i) = "mod#{i}"; end
p [Stamp, Mod, Time].map { |k| k.at(0).class }
v = [[10, 20], Stamp][ARGV.size]
p v.at(1)
w = [[10, 20], Stamp][ARGV.size + 1]
p w.at(1)
h = [{a: 1}, Stamp][ARGV.size]
p (h.at(0) rescue $!.class)
s = ["str", Stamp][ARGV.size]
p (s.at(0) rescue $!.class)
p [1, 2, 3].at(-1)
a = [1, 2]
p a.at(5)
def get(x) = x.at(0)
p get([7]), get(Stamp)
p [[1], [2]].map { |r| r.at(0) }
p (Integer.at(1) rescue $!.class)
n = [nil, Stamp][ARGV.size]
p (n.at(0) rescue $!.class)
p [3, 4].at(1.9)
p (Time.at(0).utc.year)
class Priv; class << self; private def at(x) = :p; end; end
class Sub < Stamp; end
p [Stamp, Sub].map { |k| k.at(2) }
q = [Priv, [5]][ARGV.size]
p (q.at(0) rescue $!.class)
r = [[5, 6], Priv][ARGV.size]
p r.at(-1)
x = [[1, 2], "s", Stamp, nil][ARGV.size]
p x.at(0)
y = [[1, 2], {a: 1}][ARGV.size + 1]
p (y.at(0) rescue $!.class)
z = [[1, 2], Stamp][ARGV.size]
p (z.at(0..1) rescue $!.class)
i = [3, 4, 5]
p i.at(1), i.at(-4)
s = %w[a b]
p s.at(1)
f = [1.5, 2.5]
p f.at(0)
m = [[1, "a"], Stamp][ARGV.size]
p m.at(1)
p [Stamp, Sub].map { |k| k.at(k.name.size) }
# a boxed value that is never a Class (Unused.at is never called on one)
class Unused; def self.mk(x) = "mk#{x}"; class << self; alias at mk; end; end
j = [[10, 20], 5][ARGV.size]
p j.at(1)
def g(x) = x.at(1)
p g([10, 20])
p (g({1 => 2}) rescue $!.class)
