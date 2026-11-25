# A Class.new block that runs more than declarations runs again at each
# evaluation, on the class that evaluation makes: after the superclass's
# inherited and before the call answers the class, with self (and the
# block's parameter) that class, also ahead of a def where it only passes
# the class on.
class Base
  def self.inherited(sub) = ($log << :hook; super)
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
