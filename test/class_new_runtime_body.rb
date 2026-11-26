# A Class.new block that runs more than declarations runs again at each
# evaluation, on the class that evaluation makes, before the call answers
# the class, with self (and the block's parameter) that class, also ahead of
# a def where it only passes the class on.
class Base
end
$log = []
def make = (k = Class.new(Base) { $log << :body; def w = 2 }; $log << :value; k)
a = make
b = make
p $log
p a == b, a.new.w + b.new.w

$seen = []
def tagged = Class.new { |k| $seen << k; def label = "t"; $seen << k.new.label }
x = tagged
y = tagged
p $seen.size, $seen[0] == x, $seen[2] == y, $seen[1]

$made = []
def factory = Class.new { def hi = 3; $made << new }
f = factory
g = factory
p $made.map(&:hi), $made[0].class == f, $made[1].instance_of?(g)

def mk = Class.new { def self.cm = 1 }
p mk.method(:cm).name

# the method the moved statements run in fires no definition hook
$sm = []
def mk_hooked = Class.new { def self.singleton_method_added(n) = ($sm << n); def self.x = 1; $sm << :stmt }
h1 = mk_hooked
p $sm, h1.x

# ...nor answers respond_to?
def mk_plain = Class.new { def self.y = 2; $sm << :plain }
q1 = mk_plain
q2 = mk_plain
p q1.respond_to?(:__spinel_rt_body), q2.respond_to?(:y), q1.equal?(q2), $sm.last(2)

# a class_eval block on a local holding the site's class runs with that class as self
def mk_eval(n)
  k = Class.new { def v = 1 }
  [k, k.class_eval { self == k }, k.class_exec(n) { |x| x * 2 }]
end
e1 = mk_eval(1)
e2 = mk_eval(2)
p e1[1..], e2[1..], e1[0].equal?(e2[0])
