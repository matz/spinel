# A nil message given to a reopened StandardError#initialize whose parameter is a String reads as the empty String
# (CRuby holds the nil; to_s.size is the same for both).
class StandardError
  def initialize(m = "dflt")
    super("S[#{m.to_s.size}]")
  end
end
p KeyError.new(nil, key: 1).message
p KeyError.new("abc", key: 1).message
p KeyError.new(key: 1).message
