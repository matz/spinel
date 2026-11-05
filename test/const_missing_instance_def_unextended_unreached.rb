# An instance `def const_missing` in a module nothing extends, or in a class,
# is a method a constant miss never calls: the miss asks the module object,
# whose singleton chain holds only what it extends. Reachability kept every
# def of the name for a constant read, so these methods were compiled, and the
# module's `const_get` of a run-time name was refused ("unsupported const_get
# with a name known only at run time and no class or module receiver") in a
# program that never runs it.
# CRuby: ok, Errors::ServiceError, Plain.
module Dynamic
  def const_missing(constant)
    lookup(constant)
  end

  def lookup(constant)
    const_get(constant)
  end
end

class Plain
  def const_missing(constant)
    const_get(constant)
  end
end

module Errors
  class ServiceError < StandardError; end
end

puts "ok"
puts Errors::ServiceError
puts Plain.new.class
