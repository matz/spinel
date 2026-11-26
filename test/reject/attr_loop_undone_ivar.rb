# a macro loop the runner undoes leaves self's instance variables as they were
# before it (a later attr_* on one is not read from the undone iterations), so
# that attr_* call is refused for its non-literal name
class K
  def self.remember(n)
    @last = n
  end
  def self.build
    [:first, :last].each { |n| remember(n); n.to_s }
    attr_accessor @last
  end
  build
end
p K.new.respond_to?(:last), K.new.respond_to?(:first)
