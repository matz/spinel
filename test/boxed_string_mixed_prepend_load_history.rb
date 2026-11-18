class BoxedStringMixedPrependPeer
  def size = 91
end

module BoxedStringMixedPrepend
  def size = super + 10
  def extra(value) = value + 9
end

def inherit_flag
  puts "inherit flag"
  ARGV.length > 1
end

def sample(value, name)
  p [value.size, value.respond_to?(:extra), value.respond_to?(name)]
  p [String.method_defined?(:extra), String.method_defined?(:extra, false),
     String.instance_methods.include?(:extra), String.instance_methods(false).include?(:extra)]
  p String.method_defined?(:extra, inherit_flag)
  begin
    p value.extra(2)
  rescue NoMethodError => error
    p [error.name, error.receiver.equal?(value), error.args]
  end
  begin
    p value.extra(2, 3)
  rescue NoMethodError => error
    p error.args
  rescue ArgumentError
    p :wrong_arity
  end
end

value = [+"abc", BoxedStringMixedPrependPeer.new][0]
name = ARGV.empty? ? :extra : :size
sample(value, name)
class String
  prepend BoxedStringMixedPrepend if ARGV.empty?
end
sample(value, name)
class String
  def size = 8
end
sample(value, name)
