# An alias a reopened builtin class makes more than once, one of them under
# a run-time condition: a call of the alias on a builtin value takes the
# version that ran, as a call on a program object does (#11819).
$skip = true    # the guarded code does not run
$take = false   # the guarded code runs

class Integer
  def mins = :m1
  alias minute mins
end
class Integer
  unless $take
    def mins = :m2
    alias minute mins
  end
end

class Integer
  def hrs = :h1
  alias hour hrs
end
class Integer
  unless $skip
    def hrs = :h2
    alias hour hrs
  end
end

class String
  def loud = upcase
  alias yell loud
end
unless $take
  class String
    def loud = downcase
    alias yell loud
  end
end

class Symbol
  def tone = :t1
  alias pitch tone
end
unless $skip
  class Symbol
    def tone = :t2
    alias pitch tone
  end
end

p 5.minute, 5.mins
p 5.hour, 5.hrs
p "Ab".yell
p :ab.pitch
[1, "Cd"].each { |v| p(v.is_a?(Integer) ? v.minute : v.yell) }
