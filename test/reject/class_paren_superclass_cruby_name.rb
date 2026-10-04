# RubyGems loads DidYouMean when CRuby starts, so CRuby makes Widget a
# subclass of DidYouMean::Formatter. The program does not define it, but
# CRuby does, so spinel refuses the class instead of rescuing to Base.
class Base
end

class Widget < (DidYouMean::Formatter rescue Base)
end

puts Widget.superclass
