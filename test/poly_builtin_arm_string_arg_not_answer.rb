# A String a method answers through a receiver of more than one class, where
# a class's own method answers a String it built and a builtin's arm answers
# a new one: the builtin arm read the String argument through the return
# channel, so the caller took the argument for the answer (Time#strftime
# answered the pattern beside a Date, and appending to it changed the
# pattern or raised FrozenError on it).
require "date"

def fmt(time, pattern)
  text = time.strftime(pattern)
  text
end

def stamp(time, pattern)
  text = time.strftime(pattern)
  text << "!"
end

pat = "%Y-%m-%d"
puts fmt(Time.utc(2026, 11, 10), pat)
puts fmt(Date.new(2026, 11, 10), pat)
puts stamp(Time.utc(2026, 11, 10), pat)
puts stamp(Date.new(2026, 11, 10), pat)
puts pat

class Squeezer
  def squeeze(chars)
    out = +"sq:"
    out << chars
  end
end

def squeezed(x, chars)
  text = x.squeeze(chars)
  text << "?"
end

chars = +"l"
puts squeezed("hello", chars)
puts squeezed(Squeezer.new, chars)
puts chars

class Stamp
  def strftime(fmt)
    out = +"stamp:"
    out << fmt
  end
end

def mark(time, pattern) = time.strftime(pattern)

mpat = +"%Y"
mpat << "-%m"
s = mark(Time.utc(2026, 11, 10), mpat)
s << "!"
puts s
puts mark(Stamp.new, mpat)
puts mpat
