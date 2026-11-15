# A public top-level def named new, called on an instance of a program class
# that defines no `new`, is an ordinary call: its parameter takes the call's
# argument (a class receiver's new still builds the object).
class C; end
def new(x = 1) = x
public :new
p C.new.new("s")
p C.new.new
p 3.new(4)
