# A frozen Array raises FrozenError for a mutator that adds nothing:
# push, append, unshift, prepend with no value, insert with only the
# index, concat of empty arrays, a splat that spreads no element, and
# flatten! / compact! where nothing changes. CRuby checks the receiver
# before it looks at the arguments.

def fz
  yield
  :no_error
rescue FrozenError => e
  e.message
end

ints = [6].freeze
strs = ["a"].freeze
flts = [1.5].freeze
mix = [1, "a"].freeze
nest = [[1, 2]].freeze
none = []

# the value form
p fz { ints.push }
p fz { strs.append }
p fz { flts.unshift }
p fz { mix.prepend }
p fz { nest.push }
p fz { ints.insert(0) }
p fz { strs.insert(-1) }
p fz { ints.concat([]) }
p fz { ints.concat(none, []) }
p fz { flts.concat([]) }
p fz { mix.concat([]) }
p fz { nest.concat([]) }
p fz { ints.push(*none) }
p fz { strs.insert(0, *none) }
p fz { ints.flatten! }
p fz { strs.flatten!(1) }
p fz { flts.compact! }
p fz { mix.flatten! }
p fz { mix.compact! }

# the statement form
r = []
begin; ints.concat([]); r << :no_error; rescue FrozenError; r << :frozen; end
begin; strs.push; r << :no_error; rescue FrozenError; r << :frozen; end
begin; mix.concat(none); r << :no_error; rescue FrozenError; r << :frozen; end
begin; flts.unshift(*none); r << :no_error; rescue FrozenError; r << :frozen; end
begin; ints.insert(1); r << :no_error; rescue FrozenError; r << :frozen; end
p r

# an ivar and a parameter receiver
class Holder
  def initialize; @a = [1, 2].freeze; end
  def add_none = (@a.concat([]) rescue $!.class)
end
p Holder.new.add_none
def add_none(a)
  a.push
  a.concat([])
  :no_error
rescue FrozenError
  :frozen
end
p add_none([1].freeze), add_none([1]), add_none(["a"].freeze)

# a receiver held boxed: a value that is an Array or a String
def pick(n) = n > 0 ? [1, 2].freeze : "s"
v = pick(1)
p fz { v.push }
p fz { v.insert(0) }
begin; v.push; r << :no_error; rescue FrozenError; r << :frozen; end
p r.last
held = {k: [1].freeze}
p fz { held[:k].push }

# an Array that is not frozen still answers itself, or nil for a bang
w = [3]
p w.push, w.concat([]), w.insert(0), w.push(*none), w.flatten!, w.flatten!(2), w.compact!
