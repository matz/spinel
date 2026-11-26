# method_missing in a String or an Array subclass, beside respond_to_missing?
class StringInquirer < String
  def method_missing(name, *args)
    name.end_with?("?") ? self == name[0..-2] : super
  end

  def respond_to_missing?(name, include_private = false) = name.end_with?("?") || super
end

env = StringInquirer.new("production")
p env.production?, env.development?
p env.size, env.upcase, env.class

class ArrayInquirer < Array
  def method_missing(name, *args)
    name.end_with?("?") ? any? { |s| s.to_s == name[0..-2] } : super
  end

  def respond_to_missing?(name, include_private = false) = name.end_with?("?") || super
end

pets = ArrayInquirer.new(%w[cat dog])
p pets.cat?, pets.ferret?
p pets.size, pets.first

# a String subclass without respond_to_missing?, made with and without an argument
class Tag < String
  def method_missing(name, *args) = "mm #{name}"
end

p Tag.new("t").zzz, Tag.new.size, Tag.new("ab").length

# kind queries on the instances, and a value that may be either String
p env.is_a?(String), env.instance_of?(String), env.instance_of?(StringInquirer)
p pets.instance_of?(Array), pets.instance_of?(ArrayInquirer), "plain".instance_of?(StringInquirer)
[Tag.new("a"), "b"].each do |v|
  begin
    p v.zzz
  rescue NoMethodError => e
    p e.class
  end
end

# a builtin method the subclass undefines goes to method_missing
class Short < String
  undef_method :size
  def method_missing(name, *args) = "mm #{name}"
end

p Short.new("ab").size, Short.new("ab").length

# a builtin method the subclass makes private: method_missing on another
# receiver, the builtin from inside the class
class Quiet < String
  private :upcase
  def method_missing(name, *args) = "mm #{name}"
  def shout = upcase
end

q = Quiet.new("ab")
p q.upcase, q.shout
