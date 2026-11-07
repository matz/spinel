# Thread.new hands its arguments to the block as a first Fiber#resume does:
# a splat spreads, |a| takes the first of several, |*r| takes them all, and
# one Array spreads over several parameters. The arguments were packed only
# for a block of several required parameters, a splat as one element of the
# pack, so `Thread.new(*args) { |a, b| a + b }` bound a to the whole Array
# and b to nil and raised TypeError; |a| was handed the first argument
# expression whole (the splatted Array) and the rest went unevaluated.

args = [3, 4]
p Thread.new(*args) { |a, b| a + b }.value

# |a| takes the first value, or nil
one = [5]
none = []
p Thread.new(*args) { |a| a }.value
p Thread.new(*one) { |a| a }.value
p Thread.new(*none) { |a| a }.value
p Thread.new(*[[1, 2]]) { |a| a }.value

# a splat among other arguments
p Thread.new(1, *args) { |a, b, c| [a, b, c] }.value
p Thread.new(*args, 9) { |a, b, c| [a, b, c] }.value
p Thread.new(*args, *one) { |a, b, c, d| [a, b, c, d] }.value
p Thread.new(*args, k: 2) { |a, b, h| [a, b, h] }.value

# a rest parameter takes every value
p Thread.new(1, 2) { |*r| r }.value
p Thread.new(*args) { |*r| r }.value
p Thread.new(*none) { |*r| r }.value

# one Array spreads over several parameters; a lone one keeps it
p Thread.new([1, 2]) { |a, b| [a, b] }.value
p Thread.new([1, 2]) { |a| a }.value

# a block local beside the parameters
p Thread.new(*args) { |a, b; t| t = a * 10; [t, b] }.value

# every argument is evaluated, in order
log = []
p Thread.new(log << :x, log << :y) { |a| a.size }.value
p log

# a proc handed over with &
h = ->(x, y) { x * y }
p Thread.new(*args, &h).value
p Thread.new(6, 7, &h).value

# many threads, each with its own spread arguments
ts = (1..20).map { |i| Thread.new(*[i, i * 2]) { |a, b| a + b } }
p ts.sum(&:value)

# Float values through a splat
fs = [1.5, 2.25]
p Thread.new(*fs) { |a, b| a * b }.value

# Thread.start and Thread.fork take them the same way
p Thread.start(*args) { |a, b| [a, b] }.value
p Thread.fork(1, 2) { |*r| r }.value
p Thread.start([1, 2]) { |a, b| b }.value
