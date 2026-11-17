# A prepended initialize without a keyword rest takes a `**h` that holds
# keys as one more positional Hash, which its `super` passes on: Hash's
# initialize takes it as the default, where `Hash.new(**h)` alone reads
# `capacity:` out of it. An empty `**h` passes nothing. Which one runs
# depends on the Hash, so the call is refused, not answered as keywords.
module Rest
  def initialize(*a) = super
end
Hash.prepend(Rest)
h = { capacity: 3 }
p Hash.new(**h).default
