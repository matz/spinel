# The superclass in parentheses is a method call, which spinel does not
# resolve at compile time. CRuby makes Widget a subclass of Base; spinel
# made it a subclass of Object and printed "Object".
class Base
end

def pick = Base

class Widget < (pick)
end

puts Widget.superclass
