# Bar::Inner is Foo::Inner, which Bar reaches through its superclass. The
# program can give Bar an ancestor after this point, so spinel does not
# follow ancestors for the constant before `rescue` and refuses the class.
# CRuby makes Widget a subclass of Foo::Inner.
class Base
end

class Foo
  class Inner < Base
  end
end

class Bar < Foo
end

class Widget < (Bar::Inner rescue Base)
end

puts Widget.superclass
