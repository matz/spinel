# JSON is a library name: JSON::Thing is defined when a library defines it,
# which the program's text does not say. CRuby without the library rescues
# the NameError and Widget is a subclass of Base.
class Base
end

class Widget < (JSON::Thing rescue Base)
end

puts Widget.superclass
