# spinel: gc-stress
# String#concat with several arguments takes every argument before it
# appends any, and an argument whose String a name holds is taken as a copy:
# it may be the receiver, which the first append grows. Each copy is held
# only by a C temp while the next one is made. A copy of an argument that
# ran ahead of the call into a temp of its own had no root, so under
# SPINEL_GC_STRESS=2 the next copy's allocation freed it and the append read
# the freed bytes. The operands of an argument that is a new String
# (`a + b`) were read as copies too, the first lost the same way while the
# second was made.
def fresh(n) = "f" + n.to_s
def id(x) = x
def made
  s = +"m"
  s << "k"
  s
end

class Box
  attr_accessor :s
  def initialize(s) = @s = s
end

# a value: the arguments run ahead of the call
def value_form
  l = +"la"
  a = +"aa"
  b = +"bb"
  r = l.concat(a, b, a)
  [r, l, a, b]
end
p value_form

# the receiver among its own arguments
def receiver_twice
  l = +"la"
  a = +"aa"
  r = l.concat(l, a, l)
  [r, l]
end
p receiver_twice

# a receiver that may be nil
def nil_arm(c)
  l = +"la"
  a = +"aa"
  b = +"bb"
  r = (c ? l : nil).concat(a, b, a)
  [r, l]
end
p nil_arm(true)

def either_arm(c)
  l = +"la"
  k = +"ka"
  a = +"aa"
  r = (c ? l : k).concat(a, k, l)
  [r, l, k]
end
p either_arm(true), either_arm(false)

# a receiver a bang method answers
def after_bang
  l = +"la"
  a = +"aa"
  b = +"bb"
  r = id(l).tr!("a", "o").concat(a + b, l)
  [r, l]
end
p after_bang

# elements of a String Array, as the receiver and as the arguments
def element_receiver
  ar = [+"q", +"la"]
  a = +"aa"
  b = +"bb"
  r = ar.last.concat(+a, +b)
  [r, ar]
end
p element_receiver

def element_arguments
  o = [+"m", +"n", +"o", +"q"]
  l = +"la"
  r = l.concat(o.pop, o.shift, o.pop)
  [r, l, o]
end
p element_arguments

# a reader's String, with a literal and a new String among the arguments
def reader_receiver
  bx = Box.new(+"la")
  a = +"aa"
  r = bx.s.concat(a, "lit", id(a), fresh(3))
  [r, bx.s]
end
p reader_receiver

# a String a method made, which no name holds
def made_arguments
  l = +"la"
  r = l.concat(made, made, made)
  [r, l]
end
p made_arguments

# the statement form: the operands of an argument that is a new String
def new_string_arguments
  l = +"la"
  a = +"aa"
  b = +"bb"
  l.concat(a + b, b + a, a)
  l
end
p new_string_arguments

def call_arguments
  l = +"la"
  a = +"aa"
  b = +"bb"
  r = l.concat(id(a), fresh(1), id(b), id(a))
  [r, l]
end
p call_arguments

# an instance variable and a global
class Log
  def initialize
    @s = +"<"
    @t = +"t"
  end

  def add(a)
    r = @s.concat(@t, a, @t, @s)
    [r, @s]
  end
end
p Log.new.add(+"aa")

$g = +"g"
def global_receiver
  a = +"aa"
  r = $g.concat(a, $g, a)
  [r, $g]
end
p global_receiver

# at the top level
t = +"ta"
u = +"uu"
v = +"vv"
w = t.concat(u, v, t, u)
p w, t
x = +"xa"
c = x.size > 0
y = (c ? x : nil).concat(u, v + u, x)
p y, x
