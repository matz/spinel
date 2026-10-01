# A String mutator on a conditional whose other arm returns is not moved
# into the arms: the call would land on the return, which answers no value,
# and the program would raise NoMethodError where CRuby appends. It stays
# refused, as a conditional value holding a return is.
def f(c)
  s = +"a"
  (c ? s : (return "early")) << "x"
  s
end
p f(true), f(false)
