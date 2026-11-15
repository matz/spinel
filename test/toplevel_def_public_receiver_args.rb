# A public top-level def's parameter types come from the calls that reach it
# with an explicit receiver, an object's among them, as a bare call's do.
def identity(x) = x
public :identity
class C; end
p C.new.identity("text")
p C.new.identity("text").size
p 3.identity(5)

# a class as receiver, which has no class method of the name, reaches the def
# too: its parameter takes this call's String and the others' Integers
def echo(x) = x
public :echo
p C.echo("s"), 3.echo(4), C.echo(5)
