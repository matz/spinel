# A class under a BasicObject subclass does not see Object's constants, so
# in CRuby `Gadget` raises NameError there and Widget is a subclass of Base.
# Spinel cannot tell this bare `Gadget` from a rooted `::Gadget`, so it
# refuses the class instead of using the top-level Gadget.
class Base
end

class Gadget
end

class Box < BasicObject
  class Widget < (Gadget rescue ::Base)
  end
end

puts Box::Widget.superclass
