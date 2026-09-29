# Object#method_missing, as BasicObject's: spliced by the parser when a
# program defines a method_missing of its own. A call nothing in the program
# answers is rewritten onto the hook (rewrite_method_missing_calls); a
# receiver whose class has none lands here, and so does a hook's `super`.
class Object
  def method_missing(name, *args)
    raise NoMethodError.new("undefined method '#{name}' for #{nil? ? "nil" : "an instance of #{self.class}"}", name)
  end
end

class Object
  def respond_to_missing?(name, include_all = false) = false
  # what respond_to? falls back on (see desugar_respond_to_missing): false,
  # as the default hook; a class with its own hook asks it
  def __spinel_rtm(name, include_all = false) = false
end
