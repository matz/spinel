# A yielding (inlined) method's String parameter that the body appends to is
# the caller's String, not a copy. A helper that delegated only to such a
# callee handed the caller back what its own block appended (#4476), a
# second nesting lost the outer expansion's post-yield append (#4479), and
# a callee forwarding an anonymous block with `blk.call unless blk.nil?` was
# read as letting the block escape, which celled the caller's buffer and took
# the by-reference ABI away from every append the caller made (#4477).
module Helper
  def self.tag_into(io, name)
    io << "<#{name}/>"
    nil
  end
  def self.wrap_into(io, name)
    io << "<#{name}>"
    yield if block_given?
    io << "</#{name}>"
    nil
  end
  def self.wrap_blk_into(io, name, &__blk)
    io << "<#{name}>"
    __blk.call unless __blk.nil?
    io << "</#{name}>"
    nil
  end
  def self.deep_into(io, name)
    tag_into(io, name)
    yield
    tag_into(io, name + "2")
    nil
  end
end

module Views
  def self.a_into(io); Helper.tag_into(io, "a"); nil; end
  def self.b_into(io); Helper.wrap_into(io, "b") { io << "x" }; nil; end
  def self.c_into(io); io << "["; Helper.wrap_into(io, "c") { io << "x" }; nil; end
  def self.blk_into(io, n)
    io << "["
    Helper.wrap_blk_into(io, "b") { io << n.to_s }
    io << "]"
    nil
  end
  def self.nested_into(io, n)
    io << "["
    Helper.wrap_into(io, "o") do
      io << "a"
      Helper.wrap_into(io, "i") do
        n.times { |k| io << k.to_s }
      end
      io << "b"
    end
    io << "]"
    nil
  end
  def self.run(name)
    io = String.new
    case name
    when "a" then a_into(io)
    when "b" then b_into(io)
    when "c" then c_into(io)
    when "blk" then blk_into(io, 3)
    when "nested" then nested_into(io, 2)
    end
    io
  end
end

puts Views.run("a")
puts Views.run("b")
puts Views.run("c")
puts Views.run("blk")
puts Views.run("nested")

# a plain local, a blockless call (nil? answers true), a forward to a plain
# by-reference callee from inside the expansion, and an ivar buffer
s = String.new
Helper.wrap_into(s, "p") { s << "x" }
puts s
t = String.new
Helper.wrap_blk_into(t, "n")
Helper.wrap_into(t, "m")
puts t
v = String.new
Helper.deep_into(v, "d") { v << "|" }
puts v
class Page
  def initialize; @buf = String.new; end
  def render; Helper.wrap_into(@buf, "page") { @buf << "body" }; @buf; end
end
puts Page.new.render

# a helper forwarding its anonymous block into a yielding capture, guarded
# by nil?, called with and without a block (the blockless site's forward has
# nothing to run, and is dead behind the guard)
module VH
  def self.capture
    value = yield
    value.is_a?(String) ? value.to_s : ""
  end
end
module MH
  def self.messages_tag(room, &__blk)
    "<div class=\"#{room}\">#{if __blk.nil?
      ""
    else
      VH.capture(&__blk)
    end}</div>"
  end
end
def show_into(io, room)
  io << MH.messages_tag(room) { "inner-#{room}" }
  io << MH.messages_tag("empty")
  nil
end
x = String.new
show_into(x, "r1")
puts x
