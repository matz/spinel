# frozen_string_literal: true
# With the magic comment, string literals are frozen: frozen? is true and
# mutation raises FrozenError (including through the mutable-string-buffer
# optimization, which is disabled for pragma literals).
def s(x); x; end

p "abc".frozen?            # true
p s("via method").frozen?  # true -- literal stays frozen through a call
x = "local"
p x.frozen?                # true
p "interp #{1 + 1}".frozen?  # false -- interpolation builds a new string
p "".frozen?               # true -- empty literal
p :sym.frozen?             # true (symbols always)
p [1, 2].frozen?           # false (array literal not affected by the pragma)

# dup produces an unfrozen copy
p "abc".dup.frozen?        # false

# mutating a frozen literal raises; without the pragma this local would take
# the in-place string-buffer path
buf = "abc"
begin
  buf << "y"
  puts "BUG: << did not raise"
rescue FrozenError => e
  puts e.message
end

# a dup'd copy mutates freely
cpy = "abc".dup
cpy << "y"
p cpy

# Spinel freezes the fresh Symbol#to_s result. Its dup remains mutable.
# Integer#to_s returns a mutable String under the pragma.
p :sym.to_s.dup.frozen?    # false
p 42.to_s.frozen?          # false
p __FILE__.frozen?         # true -- __FILE__ is a literal of its file
