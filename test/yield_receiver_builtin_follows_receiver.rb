# A builtin whose result follows its receiver's kind, called on a yield whose
# block answers a different kind at each call site. Each site's call is
# lowered from that site's block, so its value is boxed per site rather than
# stored into a slot typed from the first site: an Integer Array site and a
# String Array site no longer stop the build, and a Float Array site no
# longer reads back truncated to an Integer.

# element readers: the array's element
def first_of = yield.first
p first_of { [1, 2] }
p first_of { ["a", "b"] }
p first_of { [1.5, 2.5] }

def last_of = yield.last
p last_of { [1, 2] }
p last_of { ["a", "b"] }

def max_of = yield.max
p max_of { [3, 1] }
p max_of { [1.5, 2.5] }

def min_of = yield.min
p min_of { [3, 1] }
p min_of { ["b", "a"] }

def at0 = yield[0]
p at0 { [3, 1] }
p at0 { [1.5] }

def popped = yield.pop
p popped { [1, 2] }
p popped { ["x", "y"] }

def shifted = yield.shift
p shifted { [1, 2] }
p shifted { [0.5] }

# copies and reorderings: the receiver's own kind
def sorted = yield.sort
p sorted { [3, 1, 2] }
p sorted { ["b", "a"] }

def reversed = yield.reverse
p reversed { [1, 2] }
p reversed { [1.5, 2.5] }

def uniqued = yield.uniq
p uniqued { [1, 1, 2] }
p uniqued { ["a", "a"] }

def same = yield.itself
p same { 3 }
p same { 2.5 }
p same { "s" }
p same { :sym }

def copied = yield.dup
p copied { [1] }
p copied { "abc" }

def negated = yield.-@
p negated { "ab" }
p negated { 5 }
p negated { 2.5 }
p negated { "ab" }.frozen?

def mag = yield.magnitude
p mag { -3 }
p mag { -2.5 }

# the value in other positions
def into_local; x = yield.sort; x; end
p into_local { [2, 1] }
p into_local { ["d", "c"] }

def in_array = [yield.last, 0]
p in_array { [3, 1] }
p in_array { ["z"] }

def printed; puts yield.max; end
printed { [3, 1] }
printed { [2.5, 1.5] }

# a chain of them, typed per site one link at a time
def chained = yield.sort.reverse.last
p chained { [3, 1, 2] }
p chained { ["b", "c", "a"] }
p chained { [2.5, 0.5] }

def reversed_first = yield.reverse.first
p reversed_first { [1, 2] }
p reversed_first { ["a", "b"] }
