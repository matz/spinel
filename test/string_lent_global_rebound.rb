# A global handed to a parameter the method appends to, where the method
# assigns the global before it appends: the parameter is the String passed,
# so the append reaches that one, as in CRuby (it prints "new", the global's
# new String, unchanged by the append) (#6179).
def gr2(v) = ($g = +"new"; v << "x")
$g = +"g"
gr2($g)
p $g
