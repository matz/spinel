# A receiverless `send` that runs before the program's top-level `def send` is
# Object#send, which calls the method named; once the def has run it is the
# program's. The call is asked for its own point, so the same name answers
# differently on the two sides of the def, in a method body, a block or a
# begin as at the top level.

def m = :main_m
def n(x) = [:n, x]

p send(:m)
p send(:n, 1)
1.times { p send(:m) }
begin
  p send(:m)
end
p __send__(:m)
p public_send(:m) rescue p $!.class

def send(*a) = [:top, *a]

p send(:m)
p send(:n, 1)
1.times { p send(:m) }
