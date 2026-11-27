# String#=~ hands a non-Regexp operand the call: a program class's =~ answers it
class Matcher
  def =~(other)
    other.start_with?("L") ? other.size : nil
  end
end

def run(s, pats)
  pats.map do |pat|
    r = s =~ pat
    [r, $~ ? $~[0] : nil]
  end
end

p run("Line", [/i(n)/, Matcher.new, nil, /z/])
p run("xyz", [Matcher.new, /y/])

def shape(desc, criteria)
  criteria.select { |c| c.size == 2 }.collect { |regexp, pr| pr.call(desc) if desc =~ regexp }.compact
end

p shape("Line 1", [[/Line/, ->(d) { "line #{d}" }], [Matcher.new, ->(d) { "matcher #{d}" }], [/Bar/, ->(d) { "bar" }]])
p shape("x", [[Matcher.new, ->(d) { "matcher" }]])
