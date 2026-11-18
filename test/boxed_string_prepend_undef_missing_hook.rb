module StringMissingLayer
  def size = 7
end

class String
  prepend StringMissingLayer if ENV["LAYER"] == "yes"

  def respond_to_missing?(name, include_private)
    return true if name == :size && include_private
    super
  end

  def method_missing(name, *args)
    return 72 if name == :size && args.empty?
    super
  end
end

def observe_string_capture(value)
  p [value.respond_to?(:size), value.respond_to?(:size, true)]
  begin
    method = value.method(:size)
    p [:bound, method.call]
  rescue NameError
    p :absent
  end
  begin
    method = value.public_method(:size)
    p [:public, method.call]
  rescue NameError
    p :public_absent
  end
end

value = +"abc"
observe_string_capture(value)
class String
  undef_method :size
end
observe_string_capture(value)
