# instance_variable_set on a String, which CRuby gives an instance
# variable of its own: Spinel lays out no instance variables for a builtin
# value, so it is refused rather than compiled without the variable.
s = +"s"
s.instance_variable_set(:@a, 1)
p s.instance_variable_get(:@a)
