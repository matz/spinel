# `include M unless include?(M)`: Module#include leaves a module already in
# the ancestry where it is, so the guard changes nothing, and the statement
# is the include. Rails writes it in a class method (AttributeMethods#
# alias_attribute); a large application program, in class bodies and in the
# class_eval blocks of its included hooks.
module Callbacks
  def run_callbacks(kind)
    yield
  end
end

class Plain
  include Callbacks unless include?(Callbacks)
  def go = run_callbacks(:validation) { 42 }
end
p Plain.new.go
p Plain.include?(Callbacks)

# in an included hook's class_eval block, the def beside it calls the mixed-in method
module Validations
  def self.included(base)
    base.class_eval do
      include Callbacks unless include?(Callbacks)
      def _run_validation_callbacks(&block) = run_callbacks(:validation, &block)
    end
  end
end
class Model
  include Validations
end
p Model.new._run_validation_callbacks { 7 }

# the guard in a module body, the method in the module
module Guarded
  include Callbacks unless include?(Callbacks)
  def through = run_callbacks(:x) { "through" }
end
class Holder
  include Guarded
end
p Holder.new.through

# a module already included: the second statement is a no-op
class Twice
  include Callbacks
  include Callbacks unless include?(Callbacks)
  def go = run_callbacks(:x) { 1 }
end
p Twice.new.go
p Twice.ancestors.count(Callbacks)

# a qualified name
module Outer
  module Inner
    def inner = "inner"
  end
end
class Qualified
  include Outer::Inner unless include?(Outer::Inner)
end
p Qualified.new.inner

# a module already in the ancestry runs no include and no hook: the guard
# skips both, directly and through a module that includes it
module Hooked
  def self.included(base) = puts("Hooked into #{base}")
  def hooked = "hooked"
end
class Once
  include Hooked
  include Hooked unless include?(Hooked)
end
p Once.new.hooked
module Carrier
  include Hooked
end
class Via
  include Carrier
  include Hooked unless include?(Hooked)
  def both = [hooked, Via.include?(Hooked)]
end
p Via.new.both

