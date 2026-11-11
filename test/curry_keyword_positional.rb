# A curried Proc or Method hands the arguments it collected to its target
# as plain positionals, as CRuby's curry does: a trailing `k: 5` is a Hash
# among them, never the target's keywords. The target read it as keywords
# (`[1, 5]`) where CRuby raises ArgumentError for the extra positional or
# binds the Hash positionally.

def show
  yield
rescue ArgumentError => e
  e.message
end

# a lambda with a keyword param: the Hash is a second positional
f = ->(a, k: 0) { [a, k] }.curry
p show { f.(1, k: 5) }
p show { f.(1) }

# collected over several calls, the keyword lands on the last
g = ->(a, b, k: 0) { [a, b, k] }.curry
p show { g.(1).(2, k: 5) }
p show { g.(1, 2) }
p show { g.(1, k: 5) }

# a proc drops the extra positional, so its keyword stays at the default
pr = proc { |a, b, k: 0| [a, b, k] }.curry
p pr.(1).(2, k: 5)
p pr.(1, 2)

# a bound Method
def m(a, b, k: 0) = [a, b, k]
mc = method(:m).curry
p show { mc.(1).(2, k: 5) }
p mc.(1).(2)

# a **kwrest target
kr = ->(a, **kw) { [a, kw] }.curry
p show { kr.(1, z: 2) }
p kr.(1)

# through a parameter and a boxed slot
def run(c) = show { c.(1, k: 5) }
p run(->(a, k: 0) { [a, k] }.curry)
p run(->(a, b) { [a, b] }.curry)
slots = [->(a, k: 0) { [a, k] }.curry, ->(a, b) { [a, b] }.curry]
slots.each { |c| p show { c.(1, k: 5) } }

# a Hash the target takes positionally is still handed whole
h = ->(a, x) { x }.curry
p h.(1).(z: 1)
h2 = ->(a, b, k: 0) { [a, b, k] }.curry
p h2.(1, k: 5)
p h2.(1, {k: 5})

# unchanged: plain curries, optionals and a required keyword
p proc { |a, b| [a, b] }.curry[1][2]
p ->(a, b = 7, k: 0) { [a, b, k] }.curry.(1)
rk = ->(a, k:) { [a, k] }.curry
p rk.(1).class
p ->(a, b, c) { [a, b, c] }.curry.(1).(2, 3)
