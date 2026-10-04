# Later is defined only after Widget, so CRuby rescues the NameError and
# Widget is a subclass of Base. The program defines Later, but not before
# Widget, so spinel refuses the class instead of picking Later.
class Base
end

class Widget < (Later rescue Base)
end

class Later
end

puts Widget.superclass
