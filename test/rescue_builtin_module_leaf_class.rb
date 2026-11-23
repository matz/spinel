# `include IO::WaitReadable` in a program that also defines a class with
# the leaf name (`T::WaitReadable`, or a top-level `WaitWritable`) included
# that class in place of the builtin module, so `rescue IO::WaitReadable`
# missed both T::WaitReadable and T::Other, and `is_a?(IO::WaitReadable)`
# tested for T::WaitReadable.
module T
  class WaitReadable < StandardError
    include IO::WaitReadable
  end

  class Other < StandardError
    include IO::WaitReadable
  end

  class Comparable
    def initialize(n)
      @n = n
    end
    attr_reader :n
  end
end

class WaitWritable < StandardError
  include IO::WaitWritable
end

class Version
  include Comparable
  attr_reader :v
  def initialize(v)
    @v = v
  end

  def <=>(other)
    v <=> other.v
  end
end

[T::WaitReadable, T::Other].each do |klass|
  begin
    raise klass, "read would block"
  rescue IO::WaitReadable => e
    puts "#{e.class} rescued as IO::WaitReadable"
  rescue StandardError => e
    puts "#{e.class} missed by rescue IO::WaitReadable"
  end
end

begin
  raise WaitWritable, "write would block"
rescue IO::WaitWritable => e
  puts "#{e.class} rescued as IO::WaitWritable"
rescue StandardError => e
  puts "#{e.class} missed by rescue IO::WaitWritable"
end

p T::Other.new.is_a?(IO::WaitReadable)
p WaitWritable.new.is_a?(IO::WaitReadable)
p T::Comparable.new(3).n
p Version.new(1) < Version.new(2)
