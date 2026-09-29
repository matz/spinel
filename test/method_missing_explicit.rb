# A class may define method_missing and call it explicitly like any other
# method (a call nothing defines reaches it too: method_missing_unknown_names).
class Proxy
  def initialize(label)
    @label = label
  end

  def method_missing(name, *args)
    "#{@label}:#{name}/#{args.length}"
  end
end

p = Proxy.new("px")
puts p.method_missing(:foo)
puts p.method_missing(:bar, 1, 2)
