# A class value answers `class_eval` with Module's method, ahead of a def of
# the name on Kernel or Object (Active Support defines Kernel#class_eval for
# plain objects). A class held in a variable cannot run Module's, so the call
# is refused as it is with no such def; it compiled to a call of the Kernel
# def, which ran for the subclass's class_eval.
# spinel: reject-unsupported: (CallNode `class_eval`) recv=LocalVariableReadNode/ty49
module Kernel
  def class_eval(*args, &block)
    raise NotImplementedError, "Kernel#class_eval reached"
  end
end

class Base
  def self.inherited(subclass)
    super
    subclass.class_eval { @active_key = :"active_#{name.downcase}" }
  end

  def self.active_key = @active_key
end

class Alpha < Base; end
class Beta < Base; end

p Alpha.active_key
p Beta.active_key
