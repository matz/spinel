# A global handed to a yielding method's parameter that the block appends
# to, where the block assigns the global first: the parameter is the String
# passed, so the append reaches that one, as in CRuby ($z ends "other")
# (#6179).
def yl(v) = yield(v)
$z = +"z"
yl($z) { |q| $z = +"other"; q << "!" }
p $z
