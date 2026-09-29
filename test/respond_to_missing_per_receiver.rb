# A class with its own respond_to? answers for itself, while another class's
# respond_to_missing? is still consulted behind the default respond_to?.
class Only
  def respond_to_missing?(name, all = false) = name == :virt
end
class Shy
  def respond_to?(name, all = false) = false
  def respond_to_missing?(name, all = false) = true
end
s = Shy.new
o = Only.new
p s.respond_to?(:virt), o.respond_to?(:virt), o.respond_to?(:other)
