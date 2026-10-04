# More forms of a superclass in parentheses. `(X rescue Y)` is X when X is
# defined before the class (an enclosing class counts), and Y when nothing
# defines X. Nested parentheses and constant paths resolve the same way.
class Base
  def hello = "hello"
end

class Other
  def hello = "other"
end

module Mod
  class Inner
    def hello = "inner"
  end
end

class A < (Base rescue Other)
end

class B < ((Base))
end

class C < (Mod::Inner)
end

class D < (Missing rescue Mod::Inner)
end

class E < (Missing rescue (Gone::Deeper rescue ::Base))
end

class F < (Mod::Missing rescue Other)
end

class Outer
  def hello = "outer"

  class G < (Outer rescue Base)
  end
end

[A, B, C, D, E, F, Outer::G].each do |k|
  puts "#{k} < #{k.superclass}: #{k.new.hello}"
end
