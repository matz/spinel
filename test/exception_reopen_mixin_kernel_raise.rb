# A module included into a reopened StandardError: a bare raise (or puts)
# in its method is Kernel's function, not a method of the exception self.
module Tracked
  def note
    raise ArgumentError, 'boom'
  end

  def note1
    raise "plain"
  end

  def shout
    puts "from #{self.class}: #{message}"
  end
end

class StandardError
  include Tracked
end

begin
  RuntimeError.new("x").note
rescue ArgumentError => e
  puts e.message
end

begin
  KeyError.new("k").note1
rescue RuntimeError => e
  puts e.message
end

RuntimeError.new("loud").shout
