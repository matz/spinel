# Encoding-name conversion can mutate the replacement selected before it.
class Replacement
  def initialize(text)
    @text = text
  end

  def to_str
    @text
  end
end

class EncodingName
  def initialize(text)
    @text = text
  end

  def to_str
    @text.setbyte(0, 33)
    GC.start
    "US-ASCII"
  end
end

text = +"?"
name = EncodingName.new(text)
replacement = Replacement.new(text)
p ["é".encode(name, undef: :replace, replace: replacement), text]
