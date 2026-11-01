# A class object's respond_to? asked where the load has not yet defined the
# class method answers false, as CRuby does: before a `def C.z`, a
# `def self.z` in the body, a reopen's def, a class << self attr_reader or
# define_method, a module_function, a private_class_method, a superclass's
# def, an extend, define_singleton_method, instance_eval, an alias, and in a
# block or loop that runs at once. After the definition it answers true.
# Asked in a block that runs later (one a program's `each` keeps, a lazy
# chain's, an END body), it answers as of when the block runs.

class A1; end
p A1.respond_to?(:z)
def A1.z = 1
p A1.respond_to?(:z), A1.z

class A2
  p respond_to?(:z)
  def self.z = 1
  p respond_to?(:z)
end
p A2.respond_to?(:z)

class A3
  def self.a = 1
end
p A3.respond_to?(:a), A3.respond_to?(:b)
class A3
  def self.b = 2
end
p A3.respond_to?(:b), A3.b

class A4; end
p A4.respond_to?(:v)
class << A4
  attr_reader :v
end
p A4.respond_to?(:v)

module M5; end
p M5.respond_to?(:f)
module M5
  module_function def f = 1
end
p M5.respond_to?(:f), M5.f

class A6; end
p A6.respond_to?(:z, true)
class A6
  def self.z = 1
  private_class_method :z
end
p A6.respond_to?(:z), A6.respond_to?(:z, true)

class A7; end
class B7 < A7; end
p B7.respond_to?(:z)
def A7.z = 1
p B7.respond_to?(:z), B7.z

module X8; def z = 1; end
class A8; end
p A8.respond_to?(:z)
A8.extend(X8)
p A8.respond_to?(:z), A8.z

class A9; end
[1].each { p A9.respond_to?(:z) }
def A9.z = 1
[1].each { p A9.respond_to?(:z) }

class A10; end
p A10.respond_to?(:z)
A10.define_singleton_method(:z) { 1 }
p A10.respond_to?(:z), A10.z

class A11; end
p A11.respond_to?(:z)
A11.instance_eval { def z = 1 }
p A11.respond_to?(:z), A11.z

class A12; end
p A12.respond_to?(:z)
class << A12
  define_method(:z) { 1 }
end
p A12.respond_to?(:z), A12.z

class A13
  def self.a = 1
end
p A13.respond_to?(:b)
class << A13
  alias_method :b, :a
end
p A13.respond_to?(:b), A13.b

class A14; end
i = 0
while i < 1
  p A14.respond_to?(:z)
  i += 1
end
def A14.z = 1
p A14.respond_to?(:z)

# a builtin class method keeps answering before a def of its name
class A15; end
p A15.respond_to?(:name)
def A15.name = "x"
p A15.respond_to?(:name)

# a block a program's each keeps, run after the def
class A16; end
o = Object.new
def o.each(&b) = ($kept = b)
o.each { p A16.respond_to?(:z) }
def A16.z = 1
$kept.call

# a lazy chain forced after the def
class A17; end
l = [1].lazy.map { A17.respond_to?(:z) }
def A17.z = 1
p l.first

# an END body runs at exit
class A18; end
END { p A18.respond_to?(:z) }
def A18.z = 1
