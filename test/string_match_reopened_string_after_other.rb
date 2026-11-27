# A String reopening's own =~, defined after another class's, keeps the call as written
class Matcher
  def =~(other)
    "matcher"
  end
end

class String
  def =~(other)
    "string's own"
  end
end

def check(s, pats)
  pats.map { |pat| s =~ pat }
end

p check("Line", [/L/, Matcher.new])
