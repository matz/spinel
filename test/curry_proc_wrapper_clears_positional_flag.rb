# The Proc that wraps a curry (Curry#to_proc) consumes what the caller said
# of its trailing argument, as every proc's prologue does. A curry whose
# target is such a wrapper set that flag for it, and a call that stopped short
# of the saturating count answered the next curry with the flag still set for
# the next Proc call to read. Nothing here shows it in the output today: the
# Proc calls after the partial application bind the same as CRuby's, as they
# do on a build that leaves the flag set. The test pins those answers.

def show
  yield
rescue ArgumentError => e
  e.message
end

# a curry over a curry's Proc, applied short of its count
c = ->(a, b) { [a, b] }.curry.to_proc.curry
r = c.(1)
p r.class
p r.(2)

# Proc calls from runtime paths and from call sites, with a trailing Hash
pr = proc { |a, k: 0| [a, k] }
la = ->(a, k: 0) { [a, k] }
h = { k: 5 }
p pr.call(1, k: 5)
p pr.call(1, h)
p la.call(1, k: 5)
p show { la.call(1, h) }
p [[1, h]].map(&pr)
p [[1, h]].each_with_index.map { |(a, b), i| [a, b, i] }

def yield_hash = yield(1, { k: 5 })
def yield_kw = yield(1, k: 5)
p yield_hash { |a, k: 0| [a, k] }
p yield_kw { |a, k: 0| [a, k] }

# the same after a partial application of a keyword lambda's curry
kc = ->(a, b, k: 0) { [a, b, k] }.curry.to_proc.curry
kr = kc.(1)
p kr.class
p show { kr.(2, k: 3) }
p pr.call(1, k: 5)
p pr.call(1, h)
p [[1, h]].map(&pr)
p yield_hash { |a, k: 0| [a, k] }
