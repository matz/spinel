# an exception raised as a subclass and rescued as its superclass is still
# the subclass to a class test: Class#=== and case/when read its own class
# (Minitest's `Skip === failure` for a skip rescued as an Assertion)
module Mt
  class Assertion < Exception
    def label = "assertion"
  end

  class Skip < Assertion
    def label = "skip"
  end

  class Other < Assertion; end
end

def kind(e)
  case e
  when Mt::Other then :other
  when Mt::Skip then :skip
  else :none
  end
end

e = begin
  raise Mt::Skip
rescue Mt::Assertion => x
  x
end
p Mt::Skip === e, Mt::Assertion === e, Mt::Other === e, Exception === e, StandardError === e
p e.is_a?(Mt::Skip), e.class, e.label
r = case e
    when Mt::Other then :other
    when Mt::Skip then :skip
    else :none
    end
p r, kind(e)

f = begin
  raise Mt::Other
rescue Mt::Assertion => y
  y
end
p [e, f].map { |v| Mt::Skip === v }, [e, f].map { |v| kind(v) }
p({ k: e }.fetch(:k).then { |v| Mt::Skip === v })
n = nil
p Mt::Skip === n

g = begin
  raise ArgumentError, "a"
rescue StandardError => z
  z
end
p ArgumentError === g, StandardError === g, TypeError === g

# a method's answer boxed by a dispatch over the receiver's classes
class Holder
  def err
    raise Mt::Skip
  rescue Mt::Assertion => w
    w
  end
end
class Plain
  def err = 5
end
[Holder.new, Plain.new].each { |o| r = o.err; p [Mt::Skip === r, kind(r)] }
