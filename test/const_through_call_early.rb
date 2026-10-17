# A constant read through a receiver that is a call (`l.class::LEVELS`),
# in a program that may read it before it is set: as a statement (the
# compiler crashed), and in a value whose block loop is hoisted (the C
# read the slot pointer before declaring it; console's Resolver)
class Logger
end

def size_of(l)
  l.class::LEVELS.size
end

def labels(l, env = {})
  levels = l.class::LEVELS
    .map { |label, level| [level, env["CONSOLE_#{label.upcase}"]&.split(",")] }
    .to_h
    .compact
  levels
end

begin
  p size_of(Logger.new)
rescue NameError => e
  p e.message
end

class Logger
  LEVELS = {debug: 0, info: 1}
end

p size_of(Logger.new)
p labels(Logger.new)
p labels(Logger.new, {"CONSOLE_INFO" => "A,B"})
