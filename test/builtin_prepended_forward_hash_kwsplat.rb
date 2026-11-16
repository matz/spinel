# A prepended forward whose default it passes on to Hash's own initialize,
# given the call's keywords through `**h`: the Hash takes the default, and
# the keywords reach Hash#initialize after it, after the `**`'s conversion
# and the default have run.
$log = []
class Opts
  def initialize(h) = @h = h
  def to_hash = ($log << :to_hash; @h)
end
module Seeded
  def initialize(d = ($log << :d; 5), **kw) = super
end
Hash.prepend(Seeded)

h = {}
p Hash.new(**h)[:x]
p Hash.new(**{})[:x]
g = { capacity: 3 }
p Hash.new(**g)[:x]
$log = []
o = Hash.new(**Opts.new({ capacity: 2 }))
p o[:x], $log
begin
  Hash.new(**Opts.new({ k: 1 }))
rescue ArgumentError => e
  p e.message
end
p Hash.new(capacity: 2)[:x], Hash.new(1)[:x], Hash.new[:x]
