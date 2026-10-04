# CRuby loads rexml/document, which defines REXML::Element, so Widget is a
# subclass of it. Spinel ignores the require and cannot see what the
# library defines, so it refuses the class instead of rescuing to Base.
require "rexml/document"

class Base
end

class Widget < (REXML::Element rescue Base)
end

puts Widget.superclass
