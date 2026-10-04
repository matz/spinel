# Foo includes Math, so Foo::DomainError is Math::DomainError and CRuby
# makes Widget a subclass of it. The program does not define DomainError,
# but an included module can, so spinel refuses the class instead of
# rescuing to Base.
class Base
end

class Foo
  include Math
end

class Widget < (Foo::DomainError rescue Base)
end

puts Widget.superclass
