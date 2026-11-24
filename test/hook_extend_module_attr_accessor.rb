# An included hook that extends the includer with a module of attr_accessors
# and sets one through the class it is handed: the module's accessors are the
# includer's class methods, as a body's own `extend` of it makes them, and
# each includer holds its own value. The accessors were left as attr_accessor
# calls in the module's body, which gave the includer no class method, so the
# hook's `klass.allowed_tags = ...` and a later `K.allowed_tags` were refused
# as a class method no class defines.
module SafeList
  DEFAULT_ALLOWED_TAGS = ["a", "b"].freeze
  module ClassMethods
    attr_accessor :allowed_tags, :allowed_attributes
  end
  def self.included(klass)
    klass.extend(ClassMethods)
    klass.allowed_tags = DEFAULT_ALLOWED_TAGS.dup
    klass.allowed_attributes = ["href"]
  end
  def sanitize(html)
    "#{html}:#{self.class.allowed_tags.join(",")}"
  end
end
class HtmlSanitizer
  include SafeList
end
class StrictSanitizer
  include SafeList
end
StrictSanitizer.allowed_tags << "i"
StrictSanitizer.allowed_attributes = []
p HtmlSanitizer.allowed_tags, StrictSanitizer.allowed_tags
p HtmlSanitizer.allowed_attributes, StrictSanitizer.allowed_attributes
puts HtmlSanitizer.new.sanitize("<x>"), StrictSanitizer.new.sanitize("<y>")
