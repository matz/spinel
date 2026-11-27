# A bare `new` among the arguments of the one being cast takes its own cast
class Doc
  def initialize(ref = nil)
    @ref = ref
  end

  def ref
    @ref
  end

  def self.parse(s, url = nil)
    yield s if block_given?
    return new(new) if s.empty?
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

class Html5
  def self.parse(s, depth: 1)
    new(s)
  end
end

p Html5.parse("") { |x| x }.kind
p Html5.parse("b").kind
p Doc.parse("") { |x| x }.ref.kind
