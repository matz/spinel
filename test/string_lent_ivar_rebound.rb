# A top-level ivar handed to a parameter the method appends to, where the
# method assigns the ivar before it appends: the parameter is the String
# passed, so the append reaches that one, as in CRuby (it prints "new")
# (#6179).
def gri(v) = (@x = +"new"; v << "x")
@x = +"k"
gri(@x)
p @x
