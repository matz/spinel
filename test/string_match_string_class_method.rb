# String's class-side =~ is no String#=~ override: s =~ obj still hands the call to obj
class Matcher
  def =~(other)
    "matcher"
  end
end

class String
  def self.=~(other)
    "String's class"
  end
end

def check(s, pats)
  pats.map { |pat| s =~ pat }
end

p check("Line", [/i/, Matcher.new])
p(String =~ 1)
