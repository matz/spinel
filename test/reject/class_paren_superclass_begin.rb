# The BEGIN block runs before the rest of the program, so Gadget is not yet
# defined when Widget is, and in CRuby Widget is a subclass of Base. Spinel
# cannot follow the order BEGIN gives, so it refuses the class instead of
# using Gadget.
class Gadget
end

BEGIN {
  class Base
  end

  class Widget < (Gadget rescue Base)
  end
}

puts Widget.superclass
