# A super in a program's Object#respond_to_missing? reaches Kernel's, which
# answers false (the call plan's miss, not a walk that found the method
# itself and raised NoMethodError).
class Object
  def respond_to_missing?(name, include_all = false)
    name.to_s.start_with?("ghost_") || super
  end
end
class K; end
o = K.new
p o.respond_to?(:other)
p o.send(:respond_to_missing?, :zip, false)
p o.send(:respond_to_missing?, :ghost_a, false)
