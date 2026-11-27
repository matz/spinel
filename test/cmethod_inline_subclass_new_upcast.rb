# An ancestor's class method, inlined for its block with a subclass as self
# (nokogiri's XML::Document.parse for HTML5::Document): its bare `new` builds
# the subclass, which C takes into the ancestor-typed result only as cast.
class Doc
  def initialize(ref = nil)
    @ref = ref
  end

  def self.parse(s, url = nil)
    yield s if block_given?
    return new if s.empty?
    new(s)
  end

  def kind
    "doc"
  end
end

class Html5 < Doc
  def kind
    "html5"
  end
end

p Html5.parse("a").kind
p Html5.parse("", nil) { |x| x }.kind

class Html5
  def self.parse(s, depth: 1)
    new(s)
  end
end

p Html5.parse("b").kind
p Doc.parse("").kind
