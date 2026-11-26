# strip, chomp and chop on a value that can be any object, in a program
# that requires ostruct (net-ldap's ChompedIO#gets reads `@io.gets.chomp`)
require "ostruct"

class Wrap
  def initialize(v) = @v = v
  def stripped = @v.strip
  def chomped = @v.chomp
  def chopped = @v.chop
  def sized = @v.size
  def reversed = @v.reverse
  def upcased = @v.upcase
  def to_symbol = @v.to_sym
end

def wrap(v)
  v = Wrap.new(v)
  [v.stripped, v.chomped, v.chopped, v.sized, v.reversed, v.upcased, v.to_symbol]
end

def make(klass) = klass.new(1)

p wrap(" ab\n")
p wrap(OpenStruct.new(strip: 1, chomp: 2, chop: 3, size: 4, reverse: 5, upcase: 6, to_sym: 7))
