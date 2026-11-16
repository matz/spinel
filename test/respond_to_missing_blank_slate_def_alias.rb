# A conditional respond_to_missing? in a class whose superclass reaches
# BasicObject through a constant alias, a chain of them, an alias of a
# program class, a qualified path or a name two scopes carry: where the
# branch did not run there is no Kernel behind the def, so the call raises
# NoMethodError. The classes beside them that reach Object the same ways keep
# Kernel's false, and a branch that ran answers.
Base = BasicObject
B2 = Base
module A
  class B < BasicObject; end
  C = B
end
class Root < BasicObject; end
Alias = Root
class Top; end
module P
  class Top < BasicObject; end
  Kept = BasicObject
end

class X1 < Base
  if ::ARGV.size == 9
    def respond_to_missing?(n, p = false) = true
  end
  def t = respond_to_missing?(:x, false)
end
class X2 < B2
  if ::ARGV.size == 9
    def respond_to_missing?(n, p = false) = true
  end
  def t = respond_to_missing?(:x)
end
class X3 < A::B
  if ::ARGV.size == 9
    def respond_to_missing?(n, p = false) = true
  end
  def t = respond_to_missing?(:x, false)
end
class X4 < A::C
  if ::ARGV.size == 9
    def respond_to_missing?(n, p = false) = true
  end
  def t = respond_to_missing?(:x, false)
end
class X5 < Alias
  if ::ARGV.size == 9
    def respond_to_missing?(n, p = false) = true
  end
  def t = respond_to_missing?(:x, false)
end
module P
  class X6 < Top
    if ::ARGV.size == 9
      def respond_to_missing?(n, p = false) = true
    end
    def t = respond_to_missing?(:x, false)
  end
  class X7 < Kept
    if ::ARGV.size == 9
      def respond_to_missing?(n, p = false) = true
    end
    def t = respond_to_missing?(:x, false)
  end
end
class On < Base
  if ::ARGV.size == 0
    def respond_to_missing?(n, p = false) = n == :x
  end
  def t = respond_to_missing?(:x, false)
  def u = respond_to_missing?(:y)
end

class Y1 < Top
  if ARGV.size == 9
    def respond_to_missing?(n, p = false) = true
  end
end
Obj = Object
class Y2 < Obj
  if ARGV.size == 9
    def respond_to_missing?(n, p = false) = true
  end
end

def try
  ::Kernel.p yield
rescue ::NoMethodError
  ::Kernel.p :nme
end

try { X1.new.t }
try { X2.new.t }
try { X3.new.t }
try { X4.new.t }
try { X5.new.t }
try { P::X6.new.t }
try { P::X7.new.t }
try { On.new.t }
try { On.new.u }
p Y1.new.respond_to?(:zz)
p Y2.new.respond_to?(:zz)
