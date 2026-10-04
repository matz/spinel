# A call that can only raise -- a method nothing answers, or a `new` given
# a count its initialize does not take -- runs its receiver and its
# arguments first, as CRuby does: their own effects happen, and their own
# errors win. Spinel raised the call's NoMethodError or ArgumentError
# without running them.

class Styler; end
def t = Styler.new(rows).render
p((t rescue $!.class))

class S2; end
def u = S2.new(rows)
p((u rescue $!.class))

class K
  def initialize = nil
end
def noisy = (puts "ran"; K.new)
p((noisy.nope rescue $!.class))
p((S2.new(noisy) rescue $!.class))

# The staged receiver is also the exception's receiver, and a nullable
# receiver already evaluated for its error message runs only once.
begin
  noisy.nope
rescue NoMethodError => e
  p e.receiver.class
end
def noisy_string = (puts "string"; "value")
p((noisy_string.nope rescue $!.class))
