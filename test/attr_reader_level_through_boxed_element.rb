# An attr reader named like a builtin handle's (Socket::Option#level) read on a
# boxed array element: the reader switch's default arm goes through the handle
# face and must answer the same Integer-or-nil form as the class arm.
class Tok
  attr_reader :kind
  attr_reader :level

  def initialize(kind, level)
    @kind = kind
    @level = level
  end
end

class Parser
  def initialize
    @tokens = []
  end

  def parse(lines)
    lines.each do |line|
      if line.start_with?("#")
        @tokens.push(Tok.new("heading", line.length))
      else
        @tokens.push(Tok.new(line == "?" ? "heading" : "para", line == "?" ? nil : 0))
      end
    end
  end

  def tokens
    @tokens
  end
end

def render(tokens)
  out = ""
  i = 0
  while i < tokens.length
    t = tokens[i]
    if t.kind == "heading"
      ls = t.level.to_s
      out = out + "<h" + ls + ">"
    end
    i = i + 1
  end
  out
end

pr = Parser.new
pr.parse(["##", "x", "?", "#"])
puts render(pr.tokens)
