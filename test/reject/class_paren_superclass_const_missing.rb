# const_missing is defined through a symbol, so CRuby calls it for Missing
# and Widget is a subclass of Other. Spinel cannot tell what a constant
# hook answers, so it refuses the class instead of rescuing to Base.
class Base
end

class Other
end

Object.define_singleton_method(:const_missing) { |_name| Other }

class Widget < (Missing rescue Base)
end

puts Widget.superclass
