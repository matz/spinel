# Sharing a String parameter's appends used to require the method name to be
# unique in the whole program. Two things followed, and both are wrong:
#
#   1. Adding an UNUSED class with a same-named method took the ABI away from
#      a method that had it, so a callee's appends stopped reaching the
#      caller's buffer -- silently, from a class nothing instantiates (#4390).
#   2. Two methods that agree perfectly about the parameter were both refused,
#      which is the ordinary shape: a view layer with `show` in nine modules.
#
# What a call site needs is that every arm it can reach takes the parameter
# the same way (the caller's handle), whichever class defines it, and a class
# no one can instantiate is not an arm.
class HtmlWriter
  def write_to(buf)
    buf << ("<p>" * 20_000)
    nil
  end
end

class TextWriter
  def write_to(buf)
    buf << ("text" * 20_000)
    nil
  end
end

# Never instantiated: nothing can call it, so it does not hold the group back.
class UnusedWriter
  def write_to(buf)
    buf << "unused"
    nil
  end
end

a = String.new
HtmlWriter.new.write_to(a)
b = String.new
TextWriter.new.write_to(b)
puts "#{a.length} #{b.length}"

# The transitive arm through a group name too: frag_into is defined twice.
module Parts
  def self.frag_into(io, n)
    io << ("part#{n};" * 5_000)
    nil
  end
end
module Other
  def self.frag_into(io, n)
    io << ("o#{n};" * 5_000)
    nil
  end
end
module Page
  def self.body_into(io)
    Parts.frag_into(io, 1)
    Other.frag_into(io, 2)
    nil
  end
end
c = String.new
Page.body_into(c)
puts c.length
