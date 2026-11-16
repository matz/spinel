require "json"

class DuplicateDocument
  def to_json(*args)
    '{"x":1,"x":2}'
  end
end
class EscapedDocument
  def to_json(*args)
    '"' + 92.chr + 'uDC00"'
  end
end

# The generator accepts user-supplied text; the public parse defaults must
# not introduce errors in the pretty printer's internal read of that text.
[DuplicateDocument.new, EscapedDocument.new].each do |value|
  JSON.pretty_generate(value)
  puts "custom:accepted"
end
