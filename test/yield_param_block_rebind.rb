# A yielding method is spliced at its call site, and a String parameter it
# appends to, or hands to another call, is the caller's String. One the body
# only reads followed the block's rebinding of that variable: `m(u) { u =
# "k" }` showed "k" where the parameter still holds the String it was
# given. Such a parameter holds the argument's value at the call wherever
# something can assign the variable while the body runs: the call's block
# (at any depth, by any write kind), a proc that captures the variable, or,
# for a global, any write that can run then. The same holds for an instance
# variable the block assigns. Integer, Array and object parameters bind by
# value already.

def show(w)
  yield
  p w
  nil
end

def show_each(w)
  [1].each { yield }
  p w
  nil
end

def show_two(a, b)
  yield
  p a, b
  nil
end

def size_of(z) = z.size

def hand_on(w)
  yield
  p size_of(w), w
  nil
end

def show_call(w, &b)
  b.call
  p w
  nil
end

def show_yielded(w)
  yield w
  p w
  nil
end

# a plain write, an operator write, a multiple-assignment target
u = "u"
show(u) { u = "k" }
u = "u"
show(u) { u += "k" }
u = "u"
show(u) { u, _t = "k", 1 }

# the yield inside the method's own iterator, the write in a block of the
# block, two parameters, a parameter handed on, a `&b` parameter called,
# the parameter yielded to the block
u = "u"
show_each(u) { u = "k" }
u = "u"
show(u) { [1].each { u = "k" } }
u = "u"
v = "v"
show_two(u, v) { u = "k"; v = "w" }
u = "uu"
hand_on(u) { u = "k" }
u = "u"
show_call(u) { u = "k" }
u = "u"
show_yielded(u) { |x| u = "k" }

# a proc that assigns the variable, passed as the block or called from it
u = "u"
pr = proc { u = "k" }
show(u, &pr)
u = "u"
show(u) { pr.call }

# an instance variable the block assigns, and a global
class Holder
  def go
    @s = "a"
    show(@s) { @s = "b" }
    p @s
  end
end
Holder.new.go
$g = "a"
show($g) { $g = "b" }
p $g

# other kinds bind by value already
i = 1
show(i) { i = 2 }
a = [1]
show(a) { a = [2] }
