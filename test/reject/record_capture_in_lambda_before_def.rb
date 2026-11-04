# A Method of a Data class's class method made in a lambda that is called
# before the method's def raises NameError in CRuby; the compiled Method
# would bind the def. The call before the def is refused.
D = Data.define(:x)
l = -> { D.method(:z).call }
p((l.call rescue :ne))
def D.z = 3
p l.call
