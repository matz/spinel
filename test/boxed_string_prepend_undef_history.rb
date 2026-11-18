module StringUndefLayer
  def size = 7
end

def observe_string_size(value)
  p [String.instance_methods.include?(:size),
     String.instance_methods(false).include?(:size),
     String.method_defined?(:size), value.respond_to?(:size)]
  begin
    method = value.method(:size)
    p [:bound, method.call]
  rescue NameError
    p :absent
  end
end

class String
  prepend StringUndefLayer if ENV["LAYER"] == "yes"
  def size = 8
end
value = +"abc"
observe_string_size(value)

class String
  undef_method :size
end
observe_string_size(value)

class String
  def size = 9
end
observe_string_size(value)
