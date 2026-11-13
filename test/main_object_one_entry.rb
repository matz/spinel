# A top-level def and a reopening of Object's define the one entry of
# Object's method table: the later of them answers a call on main, a send
# from any receiver and a call from a method body, whichever defined it.
def run
  [m, send(:m), 1.send(:m), nil.send(:m)]
end

class Object
  def m; :object; end
end
p m
p run

def m; :main; end
p m
p run

class Object
  def m; :object2; end
end
p m
p run

class Tally
  def go; m; end
end
p Tally.new.go

i = 0
while i < 2
  p m
  def m; :main2; end
  i += 1
end
p run
p Tally.new.go

# reflection reads the same entry: the later def's visibility, and an
# alias in Object of main's private def is private
class Object; def m1; end; end
def m1; end
p Object.private_method_defined?(:m1)
class Object; alias_method :m2, :m1; end
p Object.private_method_defined?(:m2)
alias m2 hash
p Object.public_method_defined?(:m2)

# a class body's call, and a Method main makes, take the later def too,
# with the class or main as self
class Object
  def m; [:object3, self.to_s]; end
end
class Tally; p m; end
p method(:m).call
def m; :main3; end
class Tally; p m; end
p method(:m).call

# an alias on main of a builtin takes the visibility a visibility word in
# Object gave it
class Object; protected :hash; end
alias h3 hash
p Object.protected_method_defined?(:h3), Object.public_method_defined?(:h3)
