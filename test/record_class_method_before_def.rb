# A class method of a Struct or Data class, called from a method or a
# lambda before the program's def of it, raises NoMethodError, then runs
# the def once it exists. The chain past the record class is the builtin
# Struct or Data, which has no such method.

S = Struct.new(:a)
def call_s = S.late
p((call_s rescue :nme))
def S.late = 1
p call_s

D = Data.define(:x)
lam = -> { D.d_late }
p((lam.call rescue :nme))
def D.d_late = 2
p lam.call

# a header whose superclass is the class the builder makes
class T < Struct.new(:a); end
def call_t = T.t_late
p((call_t rescue :nme))
def T.t_late = 3
p call_t

# a subclass of a record class
U = Struct.new(:a)
class V < U; end
def call_v = V.u_late
p((call_v rescue :nme))
def U.u_late = 4
p call_v

# the builtin class methods answer before any def
p S.members, D.members
