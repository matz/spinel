# A String parameter a callee appends to is the caller's String, so the caller
# sees the appends. An OPTIONAL parameter beside it used to take that sharing
# away from the whole method, and the appends were dropped silently -- not always,
# only once the string outgrew its capacity, so a short append answered
# correctly and a long one did not (#4390).
#
# The exclusion was never an ABI constraint: the caller materialises every
# default, so the callee is emitted fixed-arity whether a parameter has one or
# not. Every arm here pairs a short append with a long one, because that pair
# is what separates "shared" from "copied": a copy that still has spare
# capacity answers the same as the original.

SHORT = "b"
LONG = "b" * 100_000

def fill(io, prefix = nil)
  io << prefix.to_s
  io << LONG
  nil
end

def fill_short(io, prefix = nil)
  io << prefix.to_s
  io << SHORT
  nil
end

# the control: the same method with every parameter required
def fill_required(io, prefix)
  io << prefix.to_s
  io << LONG
  nil
end

# a default taken at one site and overridden at another
def fill_n(io, n = 1)
  n.times { io << LONG }
  nil
end

# a default that IS the parameter: the caller passes a fresh unaliased temp,
# so the callee's appends go nowhere, which is what CRuby answers too
def fill_own(io = String.new)
  io << LONG
  io.size
end

# a default that READS the parameter beside it: this method keeps the value
# ABI, so the appends are the callee's own and the caller's string is untouched
def fill_reads(io, n = io.size)
  io << LONG
  n
end

def show(label, s)
  puts "#{label} #{s.size} #{s[0, 3].inspect}"
end

s = String.new; s << "a"; fill(s); show("optional default taken", s)
s = String.new; s << "a"; fill(s, "P"); show("optional default given", s)
s = String.new; s << "a"; fill_short(s); show("optional short", s)
s = String.new; s << "a"; fill_required(s, nil); show("required control", s)
s = String.new; s << "a"; fill_n(s); show("count default", s)
s = String.new; s << "a"; fill_n(s, 2); show("count given", s)

puts "own default #{fill_own}"
s = String.new; s << "a"; puts "own default given #{fill_own(s)}"; show("own default caller", s)

s = String.new; s << "a"; n = fill_reads(s); puts "reads sibling #{n}"; show("reads sibling caller", s)
