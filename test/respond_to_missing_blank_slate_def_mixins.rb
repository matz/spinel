# A respond_to_missing? defined under a run-time condition in a class under
# BasicObject: what the class mixes in decides whether Kernel's method is
# behind the def. Modules that include each other (a diamond), a core
# module other than Kernel, and Kernel mixed into the class's singleton
# (extend, an include in `class << self`) leave Kernel out of the
# instance's chain: the call raises NoMethodError where the branch did
# not run.

module A; end
module B1; include ::A; end
module B2; include ::A; end
module C1; include ::B1; include ::B2; end
module C2; include ::B1; include ::B2; end
module D; include ::C1; include ::C2; end

class X1 < BasicObject
  include ::D
  if ::ARGV.size == 9
    def respond_to_missing?(n, p = false) = true
  end
  def t = respond_to_missing?(:x, false)
end

class X2 < BasicObject
  include ::Comparable
  if ::ARGV.size == 9
    def respond_to_missing?(n, p = false) = true
  end
  def t = respond_to_missing?(:x, false)
end

class X3 < BasicObject
  extend ::Kernel
  if ::ARGV.size == 9
    def respond_to_missing?(n, p = false) = true
  end
  def t = respond_to_missing?(:x, false)
end

class X4 < BasicObject
  class << self
    include ::Kernel
  end
  if ::ARGV.size == 9
    def respond_to_missing?(n, p = false) = true
  end
  def t = respond_to_missing?(:x, false)
end

class Off < BasicObject
  if ::ARGV.size == 9
    def respond_to_missing?(n, p = false) = true
  end
  def t = respond_to_missing?(:x, false)
end
class Sub < Off
  extend ::Kernel
  def s = respond_to_missing?(:x, true)
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
try { Off.new.t }
try { Sub.new.s }
