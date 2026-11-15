# A public top-level def's parameter types come from the calls that reach it
# with an explicit receiver, an object's among them, as a bare call's do.
def identity(x) = x
public :identity
class C; end
p C.new.identity("text")
p C.new.identity("text").size
p 3.identity(5)
