class BoxedStringPrivatePrependPeer
  def size = 91
end

module BoxedStringPrivatePrepend
  private
  def size = super + 10
end

def sample(value, name)
  direct = begin
    value.size
  rescue NoMethodError => error
    [error.name, error.receiver.equal?(value)]
  end
  public_call = begin
    value.public_send(:size)
  rescue NoMethodError => error
    error.name
  end
  public_capture = begin
    value.public_method(:size).call
  rescue NameError => error
    [:hidden, error.name, error.receiver == String]
  end
  p [direct, public_call, value.send(:size), public_capture, value.method(:size).call]
  p [value.respond_to?(:size), value.respond_to?(name, true), value.respond_to?(name)]
end

value = [+"abc", BoxedStringPrivatePrependPeer.new][0]
name = ARGV.empty? ? :size : :other
sample(value, name)
class String
  prepend BoxedStringPrivatePrepend if ARGV.empty?
end
sample(value, name)
class String
  def size = 8
end
sample(value, name)
