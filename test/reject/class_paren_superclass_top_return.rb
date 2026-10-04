# The required file returns before it defines Gadget, so in CRuby `Gadget`
# raises NameError and Widget is a subclass of Base. Spinel cannot tell
# where a `return` stops the file, so it refuses the class instead of
# using Gadget.
require_relative "class_paren_superclass_top_return/gadget"

class Base
end

class Widget < (Gadget rescue Base)
end

puts Widget.superclass
