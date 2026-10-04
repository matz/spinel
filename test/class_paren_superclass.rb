# A parenthesized expression can give the superclass. `class A < (Base)`
# and `class B < (Missing rescue Base)` both make a subclass of Base, and
# the subclass inherits Base's methods. gosu-examples uses the rescue form:
# `class Tutorial < (Example rescue Gosu::Window)`.
class Base
  def hello = "hello"
end

class A < (Base)
end

class B < (Missing rescue Base)
end

puts A.superclass
puts B.superclass
puts A.new.hello
puts B.new.hello
