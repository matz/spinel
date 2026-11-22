# spinel: not-cruby -- CRuby runs Module#class_eval for the class value (Alpha.flag is :set); the block cannot be run on a class held in a boxed value, so Spinel raises NotImplementedError there instead of running the Kernel def, which is for plain objects
# A class value inside a boxed receiver answers `class_eval` with Module's
# method, ahead of a def of the name on Kernel or Object (Active Support
# defines Kernel#class_eval for plain objects). The dispatch's default arm,
# that def, is not for a class value: it raises NotImplementedError there,
# where it ran the def for the class.
module Kernel
  def class_eval(*args, &block)
    "Kernel#class_eval on #{self.class}"
  end
end

class Alpha
  def self.flag = @flag
end
class Plain; end

values = [Alpha, Plain.new]
values.each do |v|
  r = begin
    v.class_eval { @flag = :set }
  rescue NotImplementedError => e
    e.class
  end
  p r
end
p Alpha.flag
