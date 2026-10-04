# Shop::Gadget is a private constant, so CRuby's `Shop::Gadget` raises
# NameError and Widget is a subclass of Base. Spinel does not follow
# private_constant, so it refuses the class instead of using Shop::Gadget.
class Base
end

module Shop
  class Gadget
  end
  private_constant :Gadget
end

class Widget < (Shop::Gadget rescue Base)
end

puts Widget.superclass
