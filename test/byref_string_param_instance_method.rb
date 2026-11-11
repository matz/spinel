# `def add(buf, i); buf << ...; end` is ordinary Ruby: the append lands in the
# caller's string. Only free functions and class methods shared the caller's
# string, so an INSTANCE method appended to a copy and the caller's buffer stayed
# empty -- the log a benchmark built line by line came out zero-length and
# every later match found nothing.

class Builder
  def initialize
    @log = String.new
  end

  attr_reader :log

  def add(str, i)
    str << "line" << i.to_s << "\n"
  end

  def build_local(n)
    buf = String.new
    n.times { |i| add(buf, i) }
    buf
  end

  def build_ivar(n)
    @log = String.new
    n.times { |i| add(@log, i) }
    @log
  end
end

b = Builder.new
p b.build_local(3).length
p b.build_ivar(3).length
p b.log.split("\n").size
p b.build_ivar(2)
