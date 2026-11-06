# A String's each_line with a block, in a dispatch that exists because a
# class of the program defines each_line too (a buffer that hands the
# block on, and answers something else without one): the block runs over
# the lines and writes the enclosing method's locals, and the String
# answers itself.
class Buf
  def initialize(s) = @str = s
  def each_line(*args, &block)
    return @str.lines(*args) unless block
    @str.each_line(&block)
    self
  end
end

class Plain
  def each_line = :plain
end

def parse(raw)
  header = {}
  field = nil
  count = 0
  r = raw.each_line { |line|
    count += 1
    case line
    when /\A(\w+): (.*)\n\z/
      field = $1.downcase
      header[field] = $2
    else
      header[field] += line.strip
    end
  }
  [header, field, count, r.class]
end

p parse("Host: x\nAccept: a\n b\n")
p parse(Buf.new("Cookie: c=1\n"))
p Buf.new("q\nr\n").each_line
p Plain.new.each_line
