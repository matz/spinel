class Position
  def initialize(subject)
    @subject = subject
  end

  def to_int
    @subject << ("x" * 256)
    0
  end
end

regex = Regexp.new("x")
source = +"a"
p source.match?(/x/, Position.new(source))
source = +"a"
p source.match?(regex, Position.new(source))
source = +"a"
values = [source, 1]
p values[0].match?(/x/, Position.new(source))
source = +"a"
values[0] = source
p values[0].match?(regex, Position.new(source))
p source.bytesize

source = +"a"
begin
  values[1].match?(/x/, Position.new(source))
rescue NoMethodError => error
  p error.name
end
p source.bytesize

source = +"a"
saved = source
p source.match?(/x/, (source = +"other"; Position.new(saved)))
p [saved.bytesize, source]

def subject(text)
  puts "subject"
  text
end

def pattern
  puts "pattern"
  Regexp.new("x")
end

def position(text)
  puts "position"
  Position.new(text)
end

text = +"a"
p subject(text).match?(pattern, position(text))
