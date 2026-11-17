# A prepended initialize without a keyword rest takes the call's keywords as
# one more positional Hash and passes it on; the initialize after it takes
# that Hash as the parameter it lands in, and its defaults fill only the
# parameters left after it, reading it where they read that parameter.
module Inner
  def initialize(n = 1, f = n) = super
end
module Outer
  def initialize(*a) = super
end
Array.prepend(Inner)
Array.prepend(Outer)
p((Array.new(k: 1) rescue $!.class))
p Array.new
p Array.new(2)
p Array.new(2, k: 1)
