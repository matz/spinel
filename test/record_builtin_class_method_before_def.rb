# A Struct's or Data's own class method (members, keyword_init?, new)
# called from a method or a lambda before the program's def of it answers
# the builtin's; after the def, the def's.
S = Struct.new(:a, :b)
def mem = S.members
p mem
def S.members = [:mine]
p mem, S.members

K = Struct.new(:x, keyword_init: true)
kw = -> { K.keyword_init? }
p kw.()
def K.keyword_init? = :mine
p kw.()

D = Data.define(:v)
def make = D.new(1)
p make, make.v
def D.new(*a) = :mine
p make
